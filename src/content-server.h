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

#ifndef CONTENT_SERVER_H
#define CONTENT_SERVER_H

#include "object-db.h"
#include "action-log.h"
#include <set>
#include <map>
#include <boost/thread/shared_mutex.hpp>
#include <boost/thread/locks.hpp>
#include "scheduler.h"
#include <ndn-cxx/face.hpp>

class ContentServer
{
public:
  ContentServer(ActionLogPtr actionLog, const boost::filesystem::path &rootDir,
                const ndn::Name &userName, const std::string &sharedFolderName, const std::string &appName,
                int freshness = -1);
  ~ContentServer();

  // the assumption is, when the interest comes in, interest is informs of
  // /some-prefix/topology-independent-name
  // currently /topology-independent-name must begin with /action or /file
  // so that ContentServer knows where to look for the content object
  void registerPrefix(const ndn::Name &prefix);
  void deregisterPrefix(const ndn::RegisteredPrefixId &forwardingHint);
  void deregisterPrefix(const ndn::Name &forwardingHint);

private:

  void
  doNothing ();

  void
  filterAndServe (ndn::Name forwardingHint, const ndn::Name &interest);

  void
  filterAndServeImpl (const ndn::Name &forwardingHint, const ndn::Name &name, const ndn::Name &interest);

  void
  serve_Action (const ndn::Name &forwardingHint, const ndn::Name &name, const ndn::Name &interest);

  void
  serve_File (const ndn::Name &forwardingHint, const ndn::Name &name, const ndn::Name &interest);

  void
  serve_Action_Execute(const ndn::Name &forwardingHint, const ndn::Name &name, const ndn::Name &interest);

  void
  serve_File_Execute(const ndn::Name &forwardingHint, const ndn::Name &name, const ndn::Name &interest);

  void
  flushStaleDbCache();

private:
  shared_ptr<ndn::Face> m_ndn;
  ActionLogPtr m_actionLog;
  typedef boost::shared_mutex Mutex;

  typedef boost::unique_lock<Mutex> ScopedLock;
  typedef std::set<ndn::RegisteredPrefixId>::iterator PrefixIt;
  //std::set<boost::tuple<ndn::Name, ndn::RegisteredPrefixId> > m_prefixes; //TODO
  std::set<ndn::Name> m_prefixes;
  Mutex m_mutex;
  boost::filesystem::path m_dbFolder;
  int m_freshness;

  SchedulerPtr     m_scheduler;
  typedef std::map<Hash, ObjectDbPtr> DbCache;
  DbCache m_dbCache;
  Mutex m_dbCacheMutex;

  ndn::Name m_userName;
  std::string m_sharedFolderName;
  std::string m_appName;
};
#endif // CONTENT_SERVER_H
