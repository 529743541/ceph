// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "crimson/common/log.h"
#include "crimson/os/seastore/logging.h"

#include "crimson/os/seastore/lba_manager/btree/lba_btree.h"

namespace crimson::os::seastore::lba_manager::btree {

LBABtree::mkfs_ret LBABtree::mkfs(op_context_t c)
{
  auto root_leaf = c.cache.alloc_new_extent<LBALeafNode>(
    c.trans,
    LBA_BLOCK_SIZE);
  root_leaf->set_size(0);
  lba_node_meta_t meta{0, L_ADDR_MAX, 1};
  root_leaf->set_meta(meta);
  root_leaf->pin.set_range(meta);
  c.trans.get_lba_tree_stats().depth = 1u;
  return lba_root_t{root_leaf->get_paddr(), 1u};
}

LBABtree::iterator_fut LBABtree::iterator::next(
  op_context_t c,
  mapped_space_visitor_t *visitor) const
{
  assert_valid();
  assert(!is_end());

  if ((leaf.pos + 1) < leaf.node->get_size()) {
    auto ret = *this;
    ret.leaf.pos++;
    return iterator_fut(
      interruptible::ready_future_marker{},
      ret);
  }

  depth_t depth_with_space = 2;
  for (; depth_with_space <= get_depth(); ++depth_with_space) {
    if ((get_internal(depth_with_space).pos + 1) <
	get_internal(depth_with_space).node->get_size()) {
      break;
    }
  }

  if (depth_with_space <= get_depth()) {
    return seastar::do_with(
      *this,
      [](const LBAInternalNode &internal) { return internal.begin(); },
      [](const LBALeafNode &leaf) { return leaf.begin(); },
      [c, depth_with_space, visitor](auto &ret, auto &li, auto &ll) {
	for (depth_t depth = 2; depth < depth_with_space; ++depth) {
	  ret.get_internal(depth).reset();
	}
	ret.leaf.reset();
	ret.get_internal(depth_with_space).pos++;
	return lookup_depth_range(
	  c, ret, depth_with_space - 1, 0, li, ll, visitor
	).si_then([&ret] {
	  return std::move(ret);
	});
      });
  } else {
    // end
    auto ret = *this;
    ret.leaf.pos = ret.leaf.node->get_size();
    return iterator_fut(
      interruptible::ready_future_marker{},
      ret);
  }
}

LBABtree::iterator_fut LBABtree::iterator::prev(op_context_t c) const
{
  assert_valid();
  assert(!is_begin());

  auto ret = *this;

  if (is_end()) {
    ret.leaf.pos = ret.leaf.node->get_size();
  }

  if (ret.leaf.pos > 0) {
    ret.leaf.pos--;
    return iterator_fut(
      interruptible::ready_future_marker{},
      ret);
  }

  depth_t depth_with_space = 2;
  for (; depth_with_space <= get_depth(); ++depth_with_space) {
    if (ret.get_internal(depth_with_space).pos > 0) {
      break;
    }
  }

  assert(depth_with_space <= ret.get_depth()); // must not be begin()
  return seastar::do_with(
    std::move(ret),
    [](const LBAInternalNode &internal) { return --internal.end(); },
    [](const LBALeafNode &leaf) { return --leaf.end(); },
    [c, depth_with_space](auto &ret, auto &li, auto &ll) {
      for (depth_t depth = 2; depth < depth_with_space; ++depth) {
	ret.get_internal(depth).reset();
      }
      ret.leaf.reset();
      ret.get_internal(depth_with_space).pos--;
      return lookup_depth_range(
	c, ret, depth_with_space - 1, 0, li, ll, nullptr
      ).si_then([&ret] {
	return std::move(ret);
      });
    });
}

LBABtree::iterator_fut LBABtree::lower_bound(
  op_context_t c,
  laddr_t addr,
  mapped_space_visitor_t *visitor) const
{
  LOG_PREFIX(LBATree::lower_bound);
  return lookup(
    c,
    [addr](const LBAInternalNode &internal) {
      assert(internal.get_size() > 0);
      auto iter = internal.upper_bound(addr);
      assert(iter != internal.begin());
      --iter;
      return iter;
    },
    [FNAME, c, addr](const LBALeafNode &leaf) {
      auto ret = leaf.lower_bound(addr);
      DEBUGT(
	"leaf addr {}, got ret offset {}, size {}, end {}",
	c.trans,
	addr,
	ret.get_offset(),
	leaf.get_size(),
	ret == leaf.end());
      return ret;
    },
    visitor
  ).si_then([FNAME, c](auto &&ret) {
    DEBUGT(
      "ret.leaf.pos {}",
      c.trans,
      ret.leaf.pos);
    ret.assert_valid();
    return std::move(ret);
  });
}

LBABtree::insert_ret LBABtree::insert(
  op_context_t c,
  iterator iter,
  laddr_t laddr,
  lba_map_val_t val)
{
  LOG_PREFIX(LBATree::insert);
  DEBUGT(
    "inserting laddr {} at iter {}",
    c.trans,
    laddr,
    iter.is_end() ? L_ADDR_MAX : iter.get_key());
  return seastar::do_with(
    iter,
    [this, c, laddr, val](auto &ret) {
      return find_insertion(
	c, laddr, ret
      ).si_then([this, c, laddr, val, &ret] {
	if (!ret.is_end() && ret.get_key() == laddr) {
	  return insert_ret(
	    interruptible::ready_future_marker{},
	    std::make_pair(ret, false));
	} else {
	  return handle_split(
	    c, ret
	  ).si_then([c, laddr, val, &ret] {
	    if (!ret.leaf.node->is_pending()) {
	      CachedExtentRef mut = c.cache.duplicate_for_write(
		c.trans, ret.leaf.node
	      );
	      ret.leaf.node = mut->cast<LBALeafNode>();
	    }
	    auto iter = ret.leaf.node->lower_bound(laddr);
	    if (iter != ret.leaf.node->end() && iter->get_key() == laddr) {
	      return insert_ret(
		interruptible::ready_future_marker{},
		std::make_pair(ret, false));
	    } else {
	      ret.leaf.pos = iter->get_offset();
	      assert(laddr >= ret.leaf.node->get_meta().begin &&
		     laddr < ret.leaf.node->get_meta().end);
	      ret.leaf.node->insert(iter, laddr, val);
	      return insert_ret(
		interruptible::ready_future_marker{},
		std::make_pair(ret, true));
	    }
	  });
	}
      });
    });
}

LBABtree::update_ret LBABtree::update(
  op_context_t c,
  iterator iter,
  lba_map_val_t val)
{
  LOG_PREFIX(LBATree::update);
  DEBUGT(
    "update element at {}",
    c.trans,
    iter.is_end() ? L_ADDR_MAX : iter.get_key());
  if (!iter.leaf.node->is_pending()) {
    CachedExtentRef mut = c.cache.duplicate_for_write(
      c.trans, iter.leaf.node
    );
    iter.leaf.node = mut->cast<LBALeafNode>();
  }
  iter.leaf.node->update(
    iter.leaf.node->iter_idx(iter.leaf.pos),
    val);
  return update_ret(
    interruptible::ready_future_marker{},
    iter);
}

LBABtree::remove_ret LBABtree::remove(
  op_context_t c,
  iterator iter)
{
  LOG_PREFIX(LBATree::remove);
  DEBUGT(
    "remove element at {}",
    c.trans,
    iter.is_end() ? L_ADDR_MAX : iter.get_key());
  assert(!iter.is_end());
  return seastar::do_with(
    iter,
    [this, c](auto &ret) {
      if (!ret.leaf.node->is_pending()) {
	CachedExtentRef mut = c.cache.duplicate_for_write(
	  c.trans, ret.leaf.node
	);
	ret.leaf.node = mut->cast<LBALeafNode>();
      }
      ret.leaf.node->remove(
	ret.leaf.node->iter_idx(ret.leaf.pos));

      return handle_merge(
	c, ret
      );
    });
}

LBABtree::init_cached_extent_ret LBABtree::init_cached_extent(
  op_context_t c,
  CachedExtentRef e)
{
  LOG_PREFIX(LBATree::init_cached_extent);
  DEBUGT(": extent {}", c.trans, *e);
  if (e->is_logical()) {
    auto logn = e->cast<LogicalCachedExtent>();
    return lower_bound(
      c,
      logn->get_laddr()
    ).si_then([FNAME, e, c, logn](auto iter) {
      if (!iter.is_end() &&
	  iter.get_key() == logn->get_laddr() &&
	  iter.get_val().paddr == logn->get_paddr()) {
	logn->set_pin(iter.get_pin());
	ceph_assert(iter.get_val().len == e->get_length());
	c.pins.add_pin(
	  static_cast<BtreeLBAPin&>(logn->get_pin()).pin);
	DEBUGT(": logical extent {} live, initialized", c.trans, *logn);
	return e;
      } else {
	DEBUGT(": logical extent {} not live, dropping", c.trans, *logn);
	c.cache.drop_from_cache(logn);
	return CachedExtentRef();
      }
    });
  } else if (e->get_type() == extent_types_t::LADDR_INTERNAL) {
    auto eint = e->cast<LBAInternalNode>();
    return lower_bound(
      c, eint->get_node_meta().begin
    ).si_then([FNAME, e, c, eint](auto iter) {
      // Note, this check is valid even if iter.is_end()
      depth_t cand_depth = eint->get_node_meta().depth;
      if (cand_depth <= iter.get_depth() &&
	  &*iter.get_internal(cand_depth).node == &*eint) {
	DEBUGT(": extent {} is live", c.trans, *eint);
	return e;
      } else {
	DEBUGT(": extent {} is not live", c.trans, *eint);
	c.cache.drop_from_cache(eint);
	return CachedExtentRef();
      }
    });
  } else if (e->get_type() == extent_types_t::LADDR_LEAF) {
    auto eleaf = e->cast<LBALeafNode>();
    return lower_bound(
      c, eleaf->get_node_meta().begin
    ).si_then([FNAME, c, e, eleaf](auto iter) {
      // Note, this check is valid even if iter.is_end()
      if (iter.leaf.node == &*eleaf) {
	DEBUGT(": extent {} is live", c.trans, *eleaf);
	return e;
      } else {
	DEBUGT(": extent {} is not live", c.trans, *eleaf);
	c.cache.drop_from_cache(eleaf);
	return CachedExtentRef();
      }
    });
  } else {
    DEBUGT(
      ": found other extent {} type {}",
      c.trans,
      *e,
      e->get_type());
    return init_cached_extent_ret(
      interruptible::ready_future_marker{},
      e);
  }
}

LBABtree::rewrite_lba_extent_ret LBABtree::rewrite_lba_extent(
  op_context_t c,
  CachedExtentRef e)
{
  LOG_PREFIX(LBABtree::rewrite_lba_extent);
  assert(e->get_type() == extent_types_t::LADDR_INTERNAL ||
	 e->get_type() == extent_types_t::LADDR_LEAF);

  auto do_rewrite = [&](auto &lba_extent) {
    auto nlba_extent = c.cache.alloc_new_extent<
      std::remove_reference_t<decltype(lba_extent)>
      >(
      c.trans,
      lba_extent.get_length());
    lba_extent.get_bptr().copy_out(
      0,
      lba_extent.get_length(),
      nlba_extent->get_bptr().c_str());
    nlba_extent->pin.set_range(nlba_extent->get_node_meta());

    /* This is a bit underhanded.  Any relative addrs here must necessarily
     * be record relative as we are rewriting a dirty extent.  Thus, we
     * are using resolve_relative_addrs with a (likely negative) block
     * relative offset to correct them to block-relative offsets adjusted
     * for our new transaction location.
     *
     * Upon commit, these now block relative addresses will be interpretted
     * against the real final address.
     */
    nlba_extent->resolve_relative_addrs(
      make_record_relative_paddr(0) - nlba_extent->get_paddr());

    DEBUGT(
      "rewriting {} into {}",
      c.trans,
      lba_extent,
      *nlba_extent);

    return update_internal_mapping(
      c,
      nlba_extent->get_node_meta().depth,
      nlba_extent->get_node_meta().begin,
      e->get_paddr(),
      nlba_extent->get_paddr()
    ).si_then([c, e] {
      c.cache.retire_extent(c.trans, e);
    });
  };

  CachedExtentRef nlba_extent;
  if (e->get_type() == extent_types_t::LADDR_INTERNAL) {
    auto lint = e->cast<LBAInternalNode>();
    return do_rewrite(*lint);
  } else {
    assert(e->get_type() == extent_types_t::LADDR_LEAF);
    auto lleaf = e->cast<LBALeafNode>();
    return do_rewrite(*lleaf);
  }
}

LBABtree::get_internal_node_ret LBABtree::get_internal_node(
  op_context_t c,
  depth_t depth,
  paddr_t offset)
{
  LOG_PREFIX(LBATree::get_internal_node);
  DEBUGT(
    "reading internal at offset {}, depth {}",
    c.trans,
    offset,
    depth);
    return c.cache.get_extent<LBAInternalNode>(
      c.trans,
      offset,
      LBA_BLOCK_SIZE
    ).si_then([FNAME, c, offset](LBAInternalNodeRef ret) {
      DEBUGT(
	"read internal at offset {} {}",
	c.trans,
	offset,
	*ret);
      auto meta = ret->get_meta();
      if (ret->get_size()) {
	ceph_assert(meta.begin <= ret->begin()->get_key());
	ceph_assert(meta.end > (ret->end() - 1)->get_key());
      }
      if (!ret->is_pending() && !ret->pin.is_linked()) {
	ret->pin.set_range(meta);
	c.pins.add_pin(ret->pin);
      }
      return get_internal_node_ret(
	interruptible::ready_future_marker{},
	ret);
    });
}

LBABtree::get_leaf_node_ret LBABtree::get_leaf_node(
  op_context_t c,
  paddr_t offset)
{
  LOG_PREFIX(LBATree::get_leaf_node);
  DEBUGT(
    "reading leaf at offset {}",
    c.trans,
    offset);
  return c.cache.get_extent<LBALeafNode>(
    c.trans,
    offset,
    LBA_BLOCK_SIZE
  ).si_then([FNAME, c, offset](LBALeafNodeRef ret) {
    DEBUGT(
      "read leaf at offset {} {}",
      c.trans,
      offset,
      *ret);
    auto meta = ret->get_meta();
    if (ret->get_size()) {
      ceph_assert(meta.begin <= ret->begin()->get_key());
      ceph_assert(meta.end > (ret->end() - 1)->get_key());
    }
    if (!ret->is_pending() && !ret->pin.is_linked()) {
      ret->pin.set_range(meta);
      c.pins.add_pin(ret->pin);
    }
    return get_leaf_node_ret(
      interruptible::ready_future_marker{},
      ret);
  });
}

LBABtree::find_insertion_ret LBABtree::find_insertion(
  op_context_t c,
  laddr_t laddr,
  iterator &iter)
{
  assert(iter.is_end() || iter.get_key() >= laddr);
  if (!iter.is_end() && iter.get_key() == laddr) {
    return seastar::now();
  } else if (iter.leaf.node->get_node_meta().begin <= laddr) {
    auto p = iter;
    if (p.leaf.pos > 0) {
      --p.leaf.pos;
      assert(p.get_key() < laddr);
    }
    return seastar::now();
  } else {
    assert(iter.leaf.pos == 0);
    return iter.prev(
      c
    ).si_then([laddr, &iter](auto p) {
      assert(p.leaf.node->get_node_meta().begin <= laddr);
      assert(p.get_key() < laddr);
      // Note, this is specifically allowed to violate the iterator
      // invariant that pos is a valid index for the node in the event
      // that the insertion point is at the end of a node.
      p.leaf.pos++;
      iter = p;
      return seastar::now();
    });
  }
}

LBABtree::handle_split_ret LBABtree::handle_split(
  op_context_t c,
  iterator &iter)
{
  LOG_PREFIX(LBATree::insert);

  depth_t split_from = iter.check_split();

  DEBUGT("split_from {}, depth {}", c.trans, split_from, iter.get_depth());

  if (split_from == iter.get_depth()) {
    auto nroot = c.cache.alloc_new_extent<LBAInternalNode>(
      c.trans, LBA_BLOCK_SIZE);
    lba_node_meta_t meta{0, L_ADDR_MAX, iter.get_depth() + 1};
    nroot->set_meta(meta);
    nroot->pin.set_range(meta);
    nroot->journal_insert(
      nroot->begin(),
      L_ADDR_MIN,
      root.get_location(),
      nullptr);
    iter.internal.push_back({nroot, 0});

    root.set_location(nroot->get_paddr());
    root.set_depth(iter.get_depth());
    c.trans.get_lba_tree_stats().depth = iter.get_depth();
    root_dirty = true;
  }

  /* pos may be either node_position_t<LBALeafNode> or
   * node_position_t<LBAInternalNode> */
  auto split_level = [&](auto &parent_pos, auto &pos) {
    auto [left, right, pivot] = pos.node->make_split_children(c);

    auto parent_node = parent_pos.node;
    auto parent_iter = parent_pos.get_iter();

    parent_node->update(
      parent_iter,
      left->get_paddr());
    parent_node->insert(
      parent_iter + 1,
      pivot,
      right->get_paddr());

    c.cache.retire_extent(c.trans, pos.node);

    /* right->get_node_meta().begin == pivot == right->begin()->get_key()
     * Thus, if pos.pos == left->get_size(), we want iter to point to
     * left with pos.pos at the end rather than right with pos.pos = 0
     * since the insertion would be to the left of the first element
     * of right and thus necessarily less than right->get_node_meta().begin.
     */
    if (pos.pos <= left->get_size()) {
      pos.node = left;
    } else {
      pos.node = right;
      pos.pos -= left->get_size();

      parent_pos.pos += 1;
    }
  };

  for (; split_from > 0; --split_from) {
    auto &parent_pos = iter.get_internal(split_from + 1);
    if (!parent_pos.node->is_pending()) {
      parent_pos.node = c.cache.duplicate_for_write(
	c.trans, parent_pos.node
      )->cast<LBAInternalNode>();
    }

    if (split_from > 1) {
      auto &pos = iter.get_internal(split_from);
      DEBUGT("splitting parent {} depth {}", c.trans, split_from, *pos.node);
      split_level(parent_pos, pos);
    } else {
      auto &pos = iter.leaf;
      DEBUGT("splitting child {}", c.trans, *pos.node);
      split_level(parent_pos, pos);
    }
  }

  return seastar::now();
}

template <typename NodeType>
LBABtree::base_iertr::future<typename NodeType::Ref> get_node(
  op_context_t c,
  depth_t depth,
  paddr_t addr);

template <>
LBABtree::base_iertr::future<LBALeafNodeRef> get_node<LBALeafNode>(
  op_context_t c,
  depth_t depth,
  paddr_t addr) {
  assert(depth == 1);
  return LBABtree::get_leaf_node(c, addr);
}

template <>
LBABtree::base_iertr::future<LBAInternalNodeRef> get_node<LBAInternalNode>(
  op_context_t c,
  depth_t depth,
  paddr_t addr) {
  return LBABtree::get_internal_node(c, depth, addr);
}

template <typename NodeType>
LBABtree::handle_merge_ret merge_level(
  op_context_t c,
  depth_t depth,
  LBABtree::node_position_t<LBAInternalNode> &parent_pos,
  LBABtree::node_position_t<NodeType> &pos)
{
  if (!parent_pos.node->is_pending()) {
    parent_pos.node = c.cache.duplicate_for_write(
      c.trans, parent_pos.node
    )->cast<LBAInternalNode>();
  }

  auto iter = parent_pos.get_iter();
  assert(iter.get_offset() < parent_pos.node->get_size());
  bool donor_is_left = ((iter.get_offset() + 1) == parent_pos.node->get_size());
  auto donor_iter = donor_is_left ? (iter - 1) : (iter + 1);

  return get_node<NodeType>(
    c,
    depth,
    donor_iter.get_val().maybe_relative_to(parent_pos.node->get_paddr())
  ).si_then([c, iter, donor_iter, donor_is_left, &parent_pos, &pos](
	      typename NodeType::Ref donor) {
    auto [l, r] = donor_is_left ?
      std::make_pair(donor, pos.node) : std::make_pair(pos.node, donor);

    auto [liter, riter] = donor_is_left ?
      std::make_pair(donor_iter, iter) : std::make_pair(iter, donor_iter);

    if (donor->at_min_capacity()) {
      auto replacement = l->make_full_merge(c, r);

      parent_pos.node->update(
	liter,
	replacement->get_paddr());
      parent_pos.node->remove(riter);

      pos.node = replacement;
      if (donor_is_left) {
	pos.pos += r->get_size();
	parent_pos.pos--;
      }

      c.cache.retire_extent(c.trans, l);
      c.cache.retire_extent(c.trans, r);
    } else {
      auto [replacement_l, replacement_r, pivot] =
	l->make_balanced(
	  c,
	  r,
	  !donor_is_left);

      parent_pos.node->update(
	liter,
	replacement_l->get_paddr());
      parent_pos.node->replace(
	riter,
	pivot,
	replacement_r->get_paddr());

      if (donor_is_left) {
	assert(parent_pos.pos > 0);
	parent_pos.pos--;
      }

      auto orig_position = donor_is_left ?
	l->get_size() + pos.pos :
	pos.pos;
      if (orig_position < replacement_l->get_size()) {
	pos.node = replacement_l;
	pos.pos = orig_position;
      } else {
	parent_pos.pos++;
	pos.node = replacement_r;
	pos.pos = orig_position - replacement_l->get_size();
      }

      c.cache.retire_extent(c.trans, l);
      c.cache.retire_extent(c.trans, r);
    }

    return seastar::now();
  });
}

LBABtree::handle_merge_ret LBABtree::handle_merge(
  op_context_t c,
  iterator &iter)
{
  LOG_PREFIX(LBATree::handle_merge);
  if (!iter.leaf.node->at_min_capacity() ||
      iter.get_depth() == 1) {
    DEBUGT(
      "no need to merge leaf, leaf size {}, depth {}",
      c.trans,
      iter.leaf.node->get_size(),
      iter.get_depth());
    return seastar::now();
  }

  return seastar::do_with(
    depth_t{1},
    [FNAME, this, c, &iter](auto &to_merge) {
      return trans_intr::repeat(
	[FNAME, this, c, &iter, &to_merge] {
	  DEBUGT(
	    "merging depth {}",
	    c.trans,
	    to_merge);
	  auto &parent_pos = iter.get_internal(to_merge + 1);
	  auto merge_fut = handle_merge_iertr::now();
	  if (to_merge > 1) {
	    auto &pos = iter.get_internal(to_merge);
	    merge_fut = merge_level(c, to_merge, parent_pos, pos);
	  } else {
	    auto &pos = iter.leaf;
	    merge_fut = merge_level(c, to_merge, parent_pos, pos);
	  }

	  return merge_fut.si_then([FNAME, this, c, &iter, &to_merge] {
	    ++to_merge;
	    auto &pos = iter.get_internal(to_merge);
	    if (to_merge == iter.get_depth()) {
	      if (pos.node->get_size() == 1) {
		DEBUGT("collapsing root", c.trans);
		c.cache.retire_extent(c.trans, pos.node);
		assert(pos.pos == 0);
		auto node_iter = pos.get_iter();
		root.set_location(
		  node_iter->get_val().maybe_relative_to(pos.node->get_paddr()));
		iter.internal.pop_back();
		root.set_depth(iter.get_depth());
		c.trans.get_lba_tree_stats().depth = iter.get_depth();
		root_dirty = true;
	      } else {
		DEBUGT("no need to collapse root", c.trans);
	      }
	      return seastar::stop_iteration::yes;
	    } else if (pos.node->at_min_capacity()) {
	      DEBUGT(
		"continuing, next node {} depth {} at min",
		c.trans,
		*pos.node,
		to_merge);
	      return seastar::stop_iteration::no;
	    } else {
	      DEBUGT(
		"complete, next node {} depth {} not min",
		c.trans,
		*pos.node,
		to_merge);
	      return seastar::stop_iteration::yes;
	    }
	  });
	});
    });
}

LBABtree::update_internal_mapping_ret LBABtree::update_internal_mapping(
  op_context_t c,
  depth_t depth,
  laddr_t laddr,
  paddr_t old_addr,
  paddr_t new_addr)
{
  LOG_PREFIX(LBATree::update_internal_mapping);
  DEBUGT(
    "updating laddr {} at depth {} from {} to {}",
    c.trans,
    laddr,
    depth,
    old_addr,
    new_addr);

  return lower_bound(
    c, laddr
  ).si_then([=](auto iter) {
    assert(iter.get_depth() >= depth);
    if (depth == iter.get_depth()) {
      DEBUGT("update at root", c.trans);

      if (laddr != 0) {
	ERRORT(
	  "updating root laddr {} at depth {} from {} to {},"
	  "laddr is not 0",
	  c.trans,
	  laddr,
	  depth,
	  old_addr,
	  new_addr,
	  root.get_location());
	ceph_assert(0 == "impossible");
      }

      if (root.get_location() != old_addr) {
	ERRORT(
	  "updating root laddr {} at depth {} from {} to {},"
	  "root addr {} does not match",
	  c.trans,
	  laddr,
	  depth,
	  old_addr,
	  new_addr,
	  root.get_location());
	ceph_assert(0 == "impossible");
      }

      root.set_location(new_addr);
      root_dirty = true;
    } else {
      auto &parent = iter.get_internal(depth + 1);
      assert(parent.node);
      assert(parent.pos < parent.node->get_size());
      auto piter = parent.node->iter_idx(parent.pos);

      if (piter->get_key() != laddr) {
	ERRORT(
	  "updating laddr {} at depth {} from {} to {},"
	  "node {} pos {} val pivot addr {} does not match",
	  c.trans,
	  laddr,
	  depth,
	  old_addr,
	  new_addr,
	  *(parent.node),
	  parent.pos,
	  piter->get_key());
	ceph_assert(0 == "impossible");
      }


      if (piter->get_val() != old_addr) {
	ERRORT(
	  "updating laddr {} at depth {} from {} to {},"
	  "node {} pos {} val addr {} does not match",
	  c.trans,
	  laddr,
	  depth,
	  old_addr,
	  new_addr,
	  *(parent.node),
	  parent.pos,
	  piter->get_val());
	ceph_assert(0 == "impossible");
      }

      CachedExtentRef mut = c.cache.duplicate_for_write(
	c.trans,
	parent.node
      );
      LBAInternalNodeRef mparent = mut->cast<LBAInternalNode>();
      mparent->update(piter, new_addr);

      /* Note, iter is now invalid as we didn't udpate either the parent
       * node reference to the new mutable instance nor did we update the
       * child pointer to the new node.  Not a problem as we'll now just
       * destruct it.
       */
    }
    return seastar::now();
  });
}
}
