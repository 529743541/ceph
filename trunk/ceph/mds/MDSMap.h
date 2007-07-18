// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */


#ifndef __MDSMAP_H
#define __MDSMAP_H

#include "common/Clock.h"
#include "msg/Message.h"

#include "include/types.h"

#include <set>
#include <map>
#include <string>
using namespace std;


/*

  beautiful state diagram:

   STOPPED                     DNE         FAILED                    
  / |  \                      / |            |
 /  |   \________     _______/  |            |              
|   v            v   v          v            v
| STARTING <--> STANDBY <--> CREATING      REPLAY -> RECONNECT -> REJOIN 
|      \                      /                                     /
|       \____    ____________/                                    /
 \           v  v                                               /
  \         ACTIVE   <----------------------------------------/
   \          |
    \         |
     \        v
      \--  STOPPING 
               



*/


class MDSMap {
 public:
  // mds states
  static const int STATE_DNE =        0;  // down, never existed.
  static const int STATE_STOPPED =   -1;  // down, once existed, but no subtrees. empty log.
  static const int STATE_FAILED =     2;  // down, active subtrees; needs to be recovered.

  static const int STATE_BOOT     =  -3;  // up, boot announcement.  destiny unknown.
  static const int STATE_STANDBY  =  -4;  // up, idle.  waiting for assignment by monitor.
  static const int STATE_CREATING  = -5;  // up, creating MDS instance (new journal, idalloc..).
  static const int STATE_STARTING  = -6;  // up, starting prior stopped MDS instance.

  static const int STATE_REPLAY    =  7;  // up, starting prior failed instance. scanning journal.
  static const int STATE_RESOLVE   =  8;  // up, disambiguating distributed operations (import, rename, etc.)
  static const int STATE_RECONNECT =  9;  // up, reconnect to clients
  static const int STATE_REJOIN    =  10; // up, replayed journal, rejoining distributed cache
  static const int STATE_ACTIVE =     11; // up, active
  static const int STATE_STOPPING  =  12; // up, exporting metadata (-> standby or out)
  
  static const char *get_state_name(int s) {
    switch (s) {
      // down and out
    case STATE_DNE:       return "down:dne";
    case STATE_STOPPED:   return "down:stopped";
      // down and in
    case STATE_FAILED:    return "down:failed";
      // up and out
    case STATE_BOOT:      return "up:boot";
    case STATE_CREATING:  return "up:creating";
    case STATE_STARTING:  return "up:starting";
    case STATE_STANDBY:   return "up:standby";
      // up and in
    case STATE_REPLAY:    return "up:replay";
    case STATE_RESOLVE:   return "up:resolve";
    case STATE_RECONNECT: return "up:reconnect";
    case STATE_REJOIN:    return "up:rejoin";
    case STATE_ACTIVE:    return "up:active";
    case STATE_STOPPING:  return "up:stopping";
    default: assert(0);
    }
    return 0;
  }

 protected:
  epoch_t epoch;
  utime_t created;
  epoch_t same_in_set_since;  // note: this does not reflect exit-by-failure.

  int target_num;
  int anchortable;   // which MDS has anchortable (fixme someday)
  int root;          // which MDS has root directory

  set<int>               mds_created;   // which mds ids have initialized journals and id tables.
  map<int,int>           mds_state;     // MDS state
  map<int,version_t>     mds_state_seq;
  map<int,entity_inst_t> mds_inst;      // up instances
  map<int,int>           mds_inc;       // incarnation count (monotonically increases)

  friend class MDSMonitor;

 public:
  MDSMap() : epoch(0), same_in_set_since(0), anchortable(0), root(0) {}

  epoch_t get_epoch() const { return epoch; }
  void inc_epoch() { epoch++; }

  const utime_t& get_create() const { return created; }
  epoch_t get_same_in_set_since() const { return same_in_set_since; }

  int get_anchortable() const { return anchortable; }
  int get_root() const { return root; }

  // counts
  int get_num_mds() {
    return get_num_in_mds();
  }
  int get_num_mds(int state) {
    int n = 0;
    for (map<int,int>::const_iterator p = mds_state.begin();
	 p != mds_state.end();
	 p++)
      if (p->second == state) ++n;
    return n;
  }

  int get_num_in_mds() { 
    int n = 0;
    for (map<int,int>::const_iterator p = mds_state.begin();
	 p != mds_state.end();
	 p++)
      if (p->second > 0) ++n;
    return n;
  }

  // sets
  void get_mds_set(set<int>& s) {
    for (map<int,int>::const_iterator p = mds_state.begin();
	 p != mds_state.end();
	 p++)
      s.insert(p->first);
  }
  void get_mds_set(set<int>& s, int state) {
    for (map<int,int>::const_iterator p = mds_state.begin();
	 p != mds_state.end();
	 p++)
      if (p->second == state)
	s.insert(p->first);
  } 
  void get_up_mds_set(set<int>& s) {
    for (map<int,int>::const_iterator p = mds_state.begin();
	 p != mds_state.end();
	 p++)
      if (is_up(p->first)) s.insert(p->first);
  }
  void get_in_mds_set(set<int>& s) {
    for (map<int,int>::const_iterator p = mds_state.begin();
	 p != mds_state.end();
	 p++)
      if (is_in(p->first)) s.insert(p->first);
  }
  void get_active_mds_set(set<int>& s) {
    get_mds_set(s, MDSMap::STATE_ACTIVE);
  }
  void get_failed_mds_set(set<int>& s) {
    get_mds_set(s, MDSMap::STATE_FAILED);
  }
  void get_recovery_mds_set(set<int>& s) {
    for (map<int,int>::const_iterator p = mds_state.begin();
	 p != mds_state.end();
	 p++)
      if (is_failed(p->first) || 
	  (p->second >= STATE_REPLAY && p->second <= STATE_STOPPING))
	s.insert(p->first);
  }

  int get_random_in_mds() {
    vector<int> v;
    for (map<int,int>::const_iterator p = mds_state.begin();
	 p != mds_state.end();
	 p++)
      if (p->second > 0) v.push_back(p->first);
    if (v.empty())
      return -1;
    else 
      return v[rand() % v.size()];
  }


  // mds states
  bool is_down(int m) { return is_dne(m) || is_stopped(m) || is_failed(m); }
  bool is_up(int m) { return !is_down(m); }
  bool is_in(int m) { return mds_state.count(m) && mds_state[m] > 0; }
  bool is_out(int m) { return !mds_state.count(m) || mds_state[m] <= 0; }

  bool is_dne(int m)      { return mds_state.count(m) == 0 || mds_state[m] == STATE_DNE; }
  bool is_failed(int m)    { return mds_state.count(m) && mds_state[m] == STATE_FAILED; }

  bool is_boot(int m)  { return mds_state.count(m) && mds_state[m] == STATE_BOOT; }
  bool is_standby(int m)  { return mds_state.count(m) && mds_state[m] == STATE_STANDBY; }
  bool is_creating(int m) { return mds_state.count(m) && mds_state[m] == STATE_CREATING; }
  bool is_starting(int m) { return mds_state.count(m) && mds_state[m] == STATE_STARTING; }
  bool is_replay(int m)    { return mds_state.count(m) && mds_state[m] == STATE_REPLAY; }
  bool is_resolve(int m)   { return mds_state.count(m) && mds_state[m] == STATE_RESOLVE; }
  bool is_reconnect(int m) { return mds_state.count(m) && mds_state[m] == STATE_RECONNECT; }
  bool is_rejoin(int m)    { return mds_state.count(m) && mds_state[m] == STATE_REJOIN; }
  bool is_active(int m)   { return mds_state.count(m) && mds_state[m] == STATE_ACTIVE; }
  bool is_stopping(int m) { return mds_state.count(m) && mds_state[m] == STATE_STOPPING; }
  bool is_active_or_stopping(int m)   { return is_active(m) || is_stopping(m); }
  bool is_stopped(int m)  { return mds_state.count(m) && mds_state[m] == STATE_STOPPED; }

  bool has_created(int m) { return mds_created.count(m); }

  // cluster states
  bool is_full() {
    return get_num_in_mds() >= target_num;
  }
  bool is_degraded() {   // degraded = some recovery in process.  fixes active membership and recovery_set.
    return 
      get_num_mds(STATE_REPLAY) + 
      get_num_mds(STATE_RESOLVE) + 
      get_num_mds(STATE_RECONNECT) + 
      get_num_mds(STATE_REJOIN) + 
      get_num_mds(STATE_FAILED);
  }
  bool is_rejoining() {  
    // nodes are rejoining cache state
    return 
      get_num_mds(STATE_REJOIN) > 0 &&
      get_num_mds(STATE_REPLAY) == 0 &&
      get_num_mds(STATE_RECONNECT) == 0 &&
      get_num_mds(STATE_RESOLVE) == 0 &&
      get_num_mds(STATE_FAILED) == 0;
  }
  bool is_stopped() {
    return
      get_num_in_mds() == 0 &&
      get_num_mds(STATE_CREATING) == 0 &&
      get_num_mds(STATE_STARTING) == 0 &&
      get_num_mds(STATE_STANDBY) == 0;
  }


  int get_state(int m) {
    if (mds_state.count(m)) 
      return mds_state[m];
    else
      return STATE_DNE;
  }

  // inst
  bool have_inst(int m) {
    return mds_inst.count(m);
  }
  const entity_inst_t& get_inst(int m) {
    assert(mds_inst.count(m));
    return mds_inst[m];
  }
  bool get_inst(int m, entity_inst_t& inst) { 
    if (mds_inst.count(m)) {
      inst = mds_inst[m];
      return true;
    } 
    return false;
  }
  
  int get_addr_rank(const entity_addr_t& addr) {
    for (map<int,entity_inst_t>::iterator p = mds_inst.begin();
	 p != mds_inst.end();
	 ++p) {
      if (p->second.addr == addr) return p->first;
    }
    /*else
      for (map<int,entity_inst_t>::iterator p = mds_inst.begin();
	   p != mds_inst.end();
	   ++p) {
	if (memcmp(&p->second.addr,&inst.addr, sizeof(inst.addr)) == 0) return p->first;
      }
    */

    return -1;
  }

  int get_inc(int m) {
    assert(mds_inc.count(m));
    return mds_inc[m];
  }


  void remove_mds(int m) {
    mds_inst.erase(m);
    mds_state.erase(m);
    mds_state_seq.erase(m);
  }


  // serialize, unserialize
  void encode(bufferlist& bl) {
    ::_encode(epoch, bl);
    ::_encode(target_num, bl);
    ::_encode(created, bl);
    ::_encode(same_in_set_since, bl);
    ::_encode(anchortable, bl);
    ::_encode(root, bl);
    ::_encode(mds_state, bl);
    ::_encode(mds_state_seq, bl);
    ::_encode(mds_inst, bl);
    ::_encode(mds_inc, bl);
  }
  
  void decode(bufferlist& bl) {
    int off = 0;
    ::_decode(epoch, bl, off);
    ::_decode(target_num, bl, off);
    ::_decode(created, bl, off);
    ::_decode(same_in_set_since, bl, off);
    ::_decode(anchortable, bl, off);
    ::_decode(root, bl, off);
    ::_decode(mds_state, bl, off);
    ::_decode(mds_state_seq, bl, off);
    ::_decode(mds_inst, bl, off);
    ::_decode(mds_inc, bl, off);
  }


  /*** mapping functions ***/

  int hash_dentry( inodeno_t dirino, const string& dn );  
};

#endif
