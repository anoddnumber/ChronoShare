/* -*- Mode: C++; c-file-style: "gnu"; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2012-2013 University of California, Los Angeles
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
 * Author: Alexander Afanasyev <alexander.afanasyev@ucla.edu>
 *	   Zhenkai Zhu <zhenkai@cs.ucla.edu>
 */

#ifndef FETCHER_H
#define FETCHER_H

#include "executor.h"
#include <boost/intrusive/list.hpp>
#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <set>
#include <ndn-cxx/name.hpp>
#include <ndn-cxx/data.hpp>
#include <ndn-cxx/face.hpp>

class FetchManager;

class Fetcher
{
public:
  typedef boost::function<void (ndn::Name &deviceName, ndn::Name &baseName, uint64_t seq, boost::shared_ptr<ndn::Data> pco)> SegmentCallback;
  typedef boost::function<void (ndn::Name &deviceName, ndn::Name &baseName)> FinishCallback;
  typedef boost::function<void (Fetcher &, const ndn::Name &deviceName, const ndn::Name &baseName)> OnFetchCompleteCallback;
  typedef boost::function<void (Fetcher &)> OnFetchFailedCallback;

  Fetcher (ExecutorPtr executor,
           const SegmentCallback &segmentCallback, // callback passed by caller of FetchManager
           const FinishCallback &finishCallback, // callback passed by caller of FetchManager
           OnFetchCompleteCallback onFetchComplete, OnFetchFailedCallback onFetchFailed, // callbacks provided by FetchManager
           const ndn::Name &deviceName, const ndn::Name &name, int64_t minSeqNo, int64_t maxSeqNo,
           boost::posix_time::time_duration timeout = boost::posix_time::seconds (30), // this time is not precise, but sets min bound
                                                                                  // actual time depends on how fast Interests timeout
           const ndn::Name &forwardingHint = ndn::Name ());
  virtual ~Fetcher ();

  inline bool
  IsActive () const;

  inline bool
  IsTimedWait() const { return m_timedwait; }

  void
  RestartPipeline ();

  void
  SetForwardingHint (const ndn::Name &forwardingHint);

  const ndn::Name &
  GetForwardingHint () const { return m_forwardingHint; }

  const ndn::Name &
  GetName () const { return m_name; }

  const ndn::Name &
  GetDeviceName () const { return m_deviceName; }

  double
  GetRetryPause () const { return m_retryPause; }

  void
  SetRetryPause (double pause) { m_retryPause = pause; }

  boost::posix_time::ptime
  GetNextScheduledRetry () const { return m_nextScheduledRetry; }

  void
  SetNextScheduledRetry (boost::posix_time::ptime nextScheduledRetry) { m_nextScheduledRetry = nextScheduledRetry; }

private:
  void
  FillPipeline ();

  void
  OnData (uint64_t seqno, const ndn::Interest &interest, ndn::Data &data);

  void
  OnData_Execute (uint64_t seqno, const ndn::Interest &interest, ndn::Data &data);

  void
  OnTimeout (uint64_t seqno, const ndn::Interest &interest);

  void
  OnTimeout_Execute (uint64_t seqno, const ndn::Interest &interest);

public:
  boost::intrusive::list_member_hook<> m_managerListHook;

private:
  boost::shared_ptr<ndn::Face> m_ndn;

  SegmentCallback m_segmentCallback;
  OnFetchCompleteCallback m_onFetchComplete;
  OnFetchFailedCallback m_onFetchFailed;

  FinishCallback m_finishCallback;

  bool m_active;
  bool m_timedwait;

  ndn::Name m_name;
  ndn::Name m_deviceName;
  ndn::Name m_forwardingHint;

  boost::posix_time::time_duration m_maximumNoActivityPeriod;

  int64_t m_minSendSeqNo;
  int64_t m_maxInOrderRecvSeqNo;
  std::set<int64_t> m_outOfOrderRecvSeqNo;
  std::set<int64_t> m_inActivePipeline;

  int64_t m_minSeqNo;
  int64_t m_maxSeqNo;

  uint32_t m_pipeline;
  uint32_t m_activePipeline;

  boost::posix_time::ptime m_lastPositiveActivity;

  double m_retryPause; // pause to stop trying to fetch (for fetch-manager)
  boost::posix_time::ptime m_nextScheduledRetry;

  ExecutorPtr m_executor; // to serialize FillPipeline events

  boost::mutex m_seqNoMutex;
};

typedef boost::error_info<struct tag_errmsg, std::string> errmsg_info_str;

namespace Error {
struct Fetcher : virtual boost::exception, virtual std::exception { };
}

typedef boost::shared_ptr<Fetcher> FetcherPtr;

bool
Fetcher::IsActive () const
{
  return m_active;
}


#endif // FETCHER_H
