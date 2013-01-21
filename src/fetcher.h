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

#include "ccnx-wrapper.h"
#include "ccnx-name.h"

#include "scheduler.h"
#include <boost/intrusive/list.hpp>
#include <boost/date_time/posix_time/posix_time_types.hpp>

class FetchManager;

class Fetcher
{
public:
  typedef boost::function<void (Fetcher &, uint32_t /*requested seqno*/, const Ccnx::Name & /*requested base name*/,
                                const Ccnx::Name & /*actual name*/, const Ccnx::Bytes &)> OnDataSegmentCallback;
  typedef boost::function<void (Fetcher &)> OnFetchCompleteCallback;
  typedef boost::function<void (Fetcher &)> OnFetchFailedCallback;

  Fetcher (Ccnx::CcnxWrapperPtr ccnx,
           OnDataSegmentCallback onDataSegment,
           OnFetchCompleteCallback onFetchComplete, OnFetchFailedCallback onFetchFailed,
           const Ccnx::Name &name, int32_t minSeqNo, int32_t maxSeqNo,
           boost::posix_time::time_duration timeout = boost::posix_time::seconds (30), // this time is not precise, but sets min bound
                                                                                  // actual time depends on how fast Interests timeout
           const Ccnx::Name &forwardingHint = Ccnx::Name ());
  virtual ~Fetcher ();

  inline bool
  IsActive () const;

  void
  RestartPipeline ();

  void
  SetForwardingHint (const Ccnx::Name &forwardingHint);

private:
  void
  FillPipeline ();

  void
  OnData (uint32_t seqno, const Ccnx::Name &name, const Ccnx::Bytes &);

  Ccnx::Closure::TimeoutCallbackReturnValue
  OnTimeout (uint32_t seqno, const Ccnx::Name &name);

public:
  boost::intrusive::list_member_hook<> m_managerListHook;

private:
  Ccnx::CcnxWrapperPtr m_ccnx;

  OnDataSegmentCallback m_onDataSegment;
  OnFetchCompleteCallback m_onFetchComplete;
  OnFetchFailedCallback m_onFetchFailed;

  bool m_active;

  Ccnx::Name m_name;
  Ccnx::Name m_forwardingHint;

  boost::posix_time::time_duration m_maximumNoActivityPeriod;

  int32_t m_minSendSeqNo;
  int32_t m_maxInOrderRecvSeqNo;
  std::set<int32_t> m_outOfOrderRecvSeqNo;

  int32_t m_minSeqNo;
  int32_t m_maxSeqNo;

  uint32_t m_pipeline;
  uint32_t m_activePipeline;

  boost::posix_time::ptime m_lastPositiveActivity;

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