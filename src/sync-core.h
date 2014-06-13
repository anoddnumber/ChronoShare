/* -*- Mode: C++; c-file-style: "gnu"; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2013 University of California, Los Angeles
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Author: Zhenkai Zhu <zhenkai@cs.ucla.edu>
 *         Alexander Afanasyev <alexander.afanasyev@ucla.edu>
 */

#ifndef SYNC_CORE_H
#define SYNC_CORE_H

#include "sync-log.h"
#include "scheduler.h"
#include "task.h"

#include <boost/function.hpp>
#include <ndn-cxx/face.hpp>

class SyncCore
{
public:
  typedef boost::function<void (SyncStateMsgPtr stateMsg) > StateMsgCallback;

  static const int FRESHNESS = 2; // seconds
  static const string RECOVER;
  static const double WAIT; // seconds;
  static const double RANDOM_PERCENT; // seconds;

public:
  SyncCore(SyncLogPtr syncLog
           , const ndn::Name &userName
           , const ndn::Name &localPrefix      // routable name used by the local user
           , const ndn::Name &syncPrefix       // the prefix for the sync collection
           , const StateMsgCallback &callback   // callback when state change is detected
           , long syncInterestInterval = -1);
  ~SyncCore();

  void
  localStateChanged ();

  /**
   * @brief Schedule an event to update local state with a small delay
   *
   * This call is preferred to localStateChanged if many local state updates
   * are anticipated within a short period of time
   */
  void
  localStateChangedDelayed ();

  void
  updateLocalState (sqlite3_int64);

// ------------------ only used in test -------------------------
public:
  HashPtr
  root() const { return m_rootHash; }

  sqlite3_int64
  seq (const ndn::Name &name);

private:
  void
  handleInterest(const ndn::Name &name);

  void
  handleSyncData(const ndn::Interest &interest, ndn::Data &data);

  void
  handleRecoverData(const ndn::Interest &interest, ndn::Data &data);

  void
  handleSyncInterestTimeout(const ndn::Interest &interest);

  void
  handleRecoverInterestTimeout(const ndn::Interest &interest);

  void
  deregister(const ndn::Name &name);

  void
  recover(HashPtr hash);

private:
  void
  sendSyncInterest();

  void
  handleSyncInterest(const ndn::Name &name);

  void
  handleRecoverInterest(const ndn::Name &name);

  void
  handleStateData(const ndn::Buffer &content);

  void
  on_set_interest_filter_failed ();

private:
  boost::shared_ptr<ndn::Face> m_ndn;

  SyncLogPtr m_log;
  SchedulerPtr m_scheduler;
  StateMsgCallback m_stateMsgCallback;

  ndn::Name m_syncPrefix;
  HashPtr m_rootHash;

  IntervalGeneratorPtr m_recoverWaitGenerator;

  TaskPtr m_sendSyncInterestTask;

  long m_syncInterestInterval;
};

#endif // SYNC_CORE_H
