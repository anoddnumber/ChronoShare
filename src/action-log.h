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

#ifndef ACTION_LOG_H
#define ACTION_LOG_H

#include "db-helper.h"
#include "sync-log.h"
#include "action-item.pb.h"
#include "ccnx-wrapper.h"
#include "ccnx-pco.h"

#include <boost/tuple/tuple.hpp>

class ActionLog;
typedef boost::shared_ptr<ActionLog> ActionLogPtr;
typedef boost::shared_ptr<ActionItem> ActionItemPtr;

class ActionLog : public DbHelper
{
public:
  ActionLog (Ccnx::CcnxWrapperPtr ccnx, const boost::filesystem::path &path,
             SyncLogPtr syncLog,
             const std::string &sharedFolder);

  void
  AddLocalActionUpdate (const std::string &filename,
                        const Hash &hash,
                        time_t wtime,
                        int mode,
                        int seg_num);

  // void
  // AddActionMove (const std::string &oldFile, const std::string &newFile);

  void
  AddLocalActionDelete (const std::string &filename);

  bool
  KnownFileState(const std::string &filename, const Hash &hash);

  Ccnx::PcoPtr
  LookupActionPco (const Ccnx::Name &deviceName, sqlite3_int64 seqno);

  Ccnx::PcoPtr
  LookupActionPco (const Ccnx::Name &actionName);

  ActionItemPtr
  LookupAction (const Ccnx::Name &deviceName, sqlite3_int64 seqno);

  ActionItemPtr
  LookupAction (const Ccnx::Name &actionName);

public:
  // for test purposes
  sqlite3_int64
  LogSize ();

private:
  boost::tuple<sqlite3_int64 /*version*/, Ccnx::CcnxCharbufPtr /*device name*/, sqlite3_int64 /*seq_no*/>
  GetLatestActionForFile (const std::string &filename);

  static void
  apply_action_xFun (sqlite3_context *context, int argc, sqlite3_value **argv);

private:
  SyncLogPtr m_syncLog;

  Ccnx::CcnxWrapperPtr m_ccnx;
  std::string m_sharedFolderName;
};

#endif // ACTION_LOG_H
