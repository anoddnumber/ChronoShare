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

#include "action-log.h"
#include "logging.h"

#include <boost/make_shared.hpp>

using namespace boost;
using namespace std;
using namespace Ccnx;

INIT_LOGGER ("ActionLog");

const std::string INIT_DATABASE = "\
CREATE TABLE ActionLog (                                                \n\
    device_name BLOB NOT NULL,                                          \n\
    seq_no      INTEGER NOT NULL,                                       \n\
                                                                        \n\
    action      CHAR(1) NOT NULL, /* 0 for \"update\", 1 for \"delete\". */ \n\
    filename    TEXT NOT NULL,                                          \n\
                                                                        \n\
    version     INTEGER NOT NULL,                                       \n\
    action_timestamp TIMESTAMP NOT NULL,                                \n\
                                                                        \n\
    file_hash   BLOB, /* NULL if action is \"delete\" */                \n\
    file_atime  TIMESTAMP,                                              \n\
    file_mtime  TIMESTAMP,                                              \n\
    file_ctime  TIMESTAMP,                                              \n\
    file_chmod  INTEGER,                                                \n\
    file_seg_num INTEGER, /* NULL if action is \"delete\" */            \n\
                                                                        \n\
    parent_device_name BLOB,                                            \n\
    parent_seq_no      INTEGER,                                         \n\
                                                                        \n\
    action_name	     TEXT,                                              \n\
    action_content_object BLOB,                                         \n\
                                                                        \n\
    PRIMARY KEY (device_name, seq_no),                                  \n\
                                                                        \n\
    FOREIGN KEY (parent_device_name, parent_seq_no)                     \n\
	REFERENCES ActionLog (device_name, seq_no)                      \n\
	ON UPDATE RESTRICT                                              \n\
	ON DELETE SET NULL                                              \n\
);                                                                      \n\
                                                                        \n\
CREATE INDEX ActionLog_filename_version ON ActionLog (filename,version);          \n\
CREATE INDEX ActionLog_parent ON ActionLog (parent_device_name, parent_seq_no);   \n\
CREATE INDEX ActionLog_action_name ON ActionLog (action_name);          \n\
                                                                        \n\
CREATE TRIGGER ActionLogInsert_trigger                                  \n\
    AFTER INSERT ON ActionLog                                           \n\
    FOR EACH ROW                                                        \n\
    WHEN (SELECT device_name                                            \n\
            FROM ActionLog                                              \n\
            WHERE filename=NEW.filename AND                             \n\
                  version > NEW.version) IS NULL AND                    \n\
         (SELECT device_name                                            \n\
            FROM ActionLog                                              \n\
            WHERE filename=NEW.filename AND                             \n\
                  version = NEW.version AND                             \n\
                  device_name > NEW.device_name) IS NULL                \n\
    BEGIN                                                               \n\
        SELECT apply_action (NEW.device_name, NEW.seq_no,               \
                             NEW.action,NEW.filename,NEW.file_hash,     \
                             strftime('%s', NEW.file_atime),strftime('%s', NEW.file_mtime),strftime('%s', NEW.file_ctime), \
                             NEW.file_chmod, NEW.file_seg_num); /* function that applies action and adds record the FileState */  \n \
    END;                                                                \n\
                                                                        \n\
CREATE TABLE FileState (                                                \n\
    type        INTEGER NOT NULL, /* 0 - newest, 1 - oldest */          \n\
    filename    TEXT NOT NULL,                                          \n\
    device_name BLOB NOT NULL,                                          \n\
    seq_no      INTEGER NOT NULL,                                       \n\
    file_hash   BLOB NOT NULL,                                          \n\
    file_atime  TIMESTAMP,                                              \n\
    file_mtime  TIMESTAMP,                                              \n\
    file_ctime  TIMESTAMP,                                              \n\
    file_chmod  INTEGER,                                                \n\
    file_seg_num INTEGER,                                               \n\
                                                                        \n\
    PRIMARY KEY (type, filename)                                        \n\
);                                                                      \n\
                                                                        \n\
CREATE INDEX FileState_device_name_seq_no ON FileState (device_name, seq_no); \n\
CREATE INDEX FileState_type_file_hash ON FileState (type, file_hash);   \n\
";


ActionLog::ActionLog (Ccnx::CcnxWrapperPtr ccnx, const boost::filesystem::path &path,
                      SyncLogPtr syncLog,
                      const std::string &sharedFolder,
                      OnFileAddedOrChangedCallback onFileAddedOrChanged, OnFileRemovedCallback onFileRemoved)
  : DbHelper (path / ".chronoshare", "action-log.db")
  , m_syncLog (syncLog)
  , m_ccnx (ccnx)
  , m_sharedFolderName (sharedFolder)
  , m_onFileAddedOrChanged (onFileAddedOrChanged)
  , m_onFileRemoved (onFileRemoved)
{
  sqlite3_exec (m_db, INIT_DATABASE.c_str (), NULL, NULL, NULL);
  _LOG_DEBUG_COND (sqlite3_errcode (m_db) != SQLITE_OK, sqlite3_errmsg (m_db));

  int res = sqlite3_create_function (m_db, "apply_action", -1, SQLITE_ANY, reinterpret_cast<void*> (this),
                                 ActionLog::apply_action_xFun,
                                 0, 0);
  if (res != SQLITE_OK)
    {
      BOOST_THROW_EXCEPTION (Error::Db ()
                             << errmsg_info_str ("Cannot create function ``apply_action''"));
    }
}

tuple<sqlite3_int64 /*version*/, Ccnx::CcnxCharbufPtr /*device name*/, sqlite3_int64 /*seq_no*/>
ActionLog::GetLatestActionForFile (const std::string &filename)
{
  // check if something already exists
  sqlite3_stmt *stmt;
  int res = sqlite3_prepare_v2 (m_db, "SELECT version,device_name,seq_no,action "
                                "FROM ActionLog "
                                "WHERE filename=? ORDER BY version DESC LIMIT 1", -1, &stmt, 0);

  if (res != SQLITE_OK)
    {
      BOOST_THROW_EXCEPTION (Error::Db ()
                             << errmsg_info_str ("Some error with GetExistingRecord"));
    }

  sqlite3_int64 version = -1;
  CcnxCharbufPtr parent_device_name;
  sqlite3_int64 parent_seq_no = -1;

  sqlite3_bind_text (stmt, 1, filename.c_str (), filename.size (), SQLITE_STATIC);
  if (sqlite3_step (stmt) == SQLITE_ROW)
    {
      version = sqlite3_column_int64 (stmt, 0);

      if (sqlite3_column_int (stmt, 3) == 0) // prevent "linking" if the file was previously deleted
        {
          parent_device_name = make_shared<CcnxCharbuf> (sqlite3_column_blob (stmt, 1), sqlite3_column_bytes (stmt, 1));
          parent_seq_no = sqlite3_column_int64 (stmt, 2);
        }
    }

  sqlite3_finalize (stmt);
  return make_tuple (version, parent_device_name, parent_seq_no);
}

// local add action. remote action is extracted from content object
ActionItemPtr
ActionLog::AddLocalActionUpdate (const std::string &filename,
                                 const Hash &hash,
                                 time_t wtime,
                                 int mode,
                                 int seg_num)
{
  sqlite3_exec (m_db, "BEGIN TRANSACTION;", 0,0,0);

  CcnxCharbufPtr device_name = m_syncLog->GetLocalName ().toCcnxCharbuf ();
  sqlite3_int64  seq_no = m_syncLog->GetNextLocalSeqNo ();
  sqlite3_int64  version;
  CcnxCharbufPtr parent_device_name;
  sqlite3_int64  parent_seq_no = -1;

  sqlite3_int64  action_time = time (0);

  tie (version, parent_device_name, parent_seq_no) = GetLatestActionForFile (filename);
  version ++;

  sqlite3_stmt *stmt;
  int res = sqlite3_prepare_v2 (m_db, "INSERT INTO ActionLog "
                                "(device_name, seq_no, action, filename, version, action_timestamp, "
                                "file_hash, file_atime, file_mtime, file_ctime, file_chmod, file_seg_num, "
                                "parent_device_name, parent_seq_no, "
                                "action_name, action_content_object) "
                                "VALUES (?, ?, ?, ?, ?, datetime(?, 'unixepoch'),"
                                "        ?, datetime(?, 'unixepoch'), datetime(?, 'unixepoch'), datetime(?, 'unixepoch'), ?,?, "
                                "        ?, ?, "
                                "        ?, ?);", -1, &stmt, 0);

  _LOG_DEBUG_COND (sqlite3_errcode (m_db) != SQLITE_OK, sqlite3_errmsg (m_db));

  if (res != SQLITE_OK)
    {
      BOOST_THROW_EXCEPTION (Error::Db ()
                             << errmsg_info_str (sqlite3_errmsg (m_db))
                             );
    }

  sqlite3_bind_blob  (stmt, 1, device_name->buf (), device_name->length (), SQLITE_TRANSIENT);
  sqlite3_bind_int64 (stmt, 2, seq_no);
  sqlite3_bind_int   (stmt, 3, 0);
  sqlite3_bind_text  (stmt, 4, filename.c_str (), filename.size (), SQLITE_TRANSIENT);
  sqlite3_bind_int64 (stmt, 5, version);
  sqlite3_bind_int64 (stmt, 6, action_time);

  sqlite3_bind_blob  (stmt, 7, hash.GetHash (), hash.GetHashBytes (), SQLITE_TRANSIENT);

  // sqlite3_bind_int64 (stmt, 8, atime); // NULL
  sqlite3_bind_int64 (stmt, 9, wtime);
  // sqlite3_bind_int64 (stmt, 10, ctime); // NULL
  sqlite3_bind_int   (stmt, 11, mode);
  sqlite3_bind_int   (stmt, 12, seg_num);

  if (parent_device_name && parent_seq_no > 0)
    {
      sqlite3_bind_blob (stmt, 13, parent_device_name->buf (), parent_device_name->length (), SQLITE_TRANSIENT);
      sqlite3_bind_int64 (stmt, 14, parent_seq_no);
    }

  ActionItemPtr item = make_shared<ActionItem> ();
  item->set_action (ActionItem::UPDATE);
  item->set_filename (filename);
  item->set_version (version);
  item->set_timestamp (action_time);
  item->set_file_hash (hash.GetHash (), hash.GetHashBytes ());
  // item->set_atime (atime);
  item->set_mtime (wtime);
  // item->set_ctime (ctime);
  item->set_mode (mode);
  item->set_seg_num (seg_num);

  if (parent_device_name && parent_seq_no > 0)
    {
      // cout << Name (*parent_device_name) << endl;

      item->set_parent_device_name (parent_device_name->buf (), parent_device_name->length ());
      item->set_parent_seq_no (parent_seq_no);
    }

  // assign name to the action, serialize action, and create content object

  string item_msg;
  item->SerializeToString (&item_msg);
  Name actionName = Name (m_syncLog->GetLocalName ())("action")(m_sharedFolderName)(seq_no);
  _LOG_DEBUG ("ActionName: " << actionName);

  Bytes actionData = m_ccnx->createContentObject (actionName, item_msg.c_str (), item_msg.size ());
  CcnxCharbufPtr namePtr = actionName.toCcnxCharbuf ();

  // _LOG_DEBUG (" >>>>>>> " << Name (namePtr->buf () << " " << namePtr->length ());

  sqlite3_bind_blob (stmt, 15, namePtr->buf (), namePtr->length (), SQLITE_TRANSIENT);
  sqlite3_bind_blob (stmt, 16, head (actionData), actionData.size (), SQLITE_TRANSIENT);

  sqlite3_step (stmt);

  _LOG_DEBUG_COND (sqlite3_errcode (m_db) != SQLITE_OK && sqlite3_errcode (m_db) != SQLITE_ROW, sqlite3_errmsg (m_db));

  sqlite3_finalize (stmt);

  sqlite3_exec (m_db, "END TRANSACTION;", 0,0,0);

  return item;
}

// void
// ActionLog::AddActionMove (const std::string &oldFile, const std::string &newFile)
// {
//   // not supported yet
//   BOOST_THROW_EXCEPTION (Error::Db ()
//                          << errmsg_info_str ("Move operation is not yet supported"));
// }

ActionItemPtr
ActionLog::AddLocalActionDelete (const std::string &filename)
{
  sqlite3_exec (m_db, "BEGIN TRANSACTION;", 0,0,0);

  CcnxCharbufPtr device_name = m_syncLog->GetLocalName ().toCcnxCharbuf ();
  sqlite3_int64  version;
  CcnxCharbufPtr parent_device_name;
  sqlite3_int64  parent_seq_no = -1;

  sqlite3_int64 action_time = time (0);

  tie (version, parent_device_name, parent_seq_no) = GetLatestActionForFile (filename);
  if (!parent_device_name) // no records exist or file was already deleted
    {
      sqlite3_exec (m_db, "END TRANSACTION;", 0,0,0);
      return ActionItemPtr ();
    }
  version ++;

  sqlite3_int64 seq_no = m_syncLog->GetNextLocalSeqNo ();

  sqlite3_stmt *stmt;
  sqlite3_prepare_v2 (m_db, "INSERT INTO ActionLog "
                      "(device_name, seq_no, action, filename, version, action_timestamp, "
                      "parent_device_name, parent_seq_no, "
                      "action_name, action_content_object) "
                      "VALUES (?, ?, ?, ?, ?, datetime(?, 'unixepoch'),"
                      "        ?, ?,"
                      "        ?, ?)", -1, &stmt, 0);

  sqlite3_bind_blob  (stmt, 1, device_name->buf (), device_name->length (), SQLITE_TRANSIENT);
  sqlite3_bind_int64 (stmt, 2, seq_no);
  sqlite3_bind_int   (stmt, 3, 1);
  sqlite3_bind_text  (stmt, 4, filename.c_str (), filename.size (), SQLITE_TRANSIENT);
  sqlite3_bind_int64 (stmt, 5, version);
  sqlite3_bind_int64 (stmt, 6, action_time);

  sqlite3_bind_blob  (stmt, 7, parent_device_name->buf (), parent_device_name->length (), SQLITE_TRANSIENT);
  sqlite3_bind_int64 (stmt, 8, parent_seq_no);


  ActionItemPtr item = make_shared<ActionItem> ();
  item->set_action (ActionItem::DELETE);
  item->set_filename (filename);
  item->set_version (version);
  item->set_timestamp (action_time);
  item->set_parent_device_name (parent_device_name->buf (), parent_device_name->length ());
  item->set_parent_seq_no (parent_seq_no);

  string item_msg;
  item->SerializeToString (&item_msg);
  Name actionName = Name (m_syncLog->GetLocalName ())("action")(m_sharedFolderName)(seq_no);

  Bytes actionData = m_ccnx->createContentObject (actionName, item_msg.c_str (), item_msg.size ());
  CcnxCharbufPtr namePtr = actionName.toCcnxCharbuf ();

  sqlite3_bind_blob (stmt, 9, namePtr->buf (), namePtr->length (), SQLITE_TRANSIENT);
  sqlite3_bind_blob (stmt, 10, &actionData[0], actionData.size (), SQLITE_TRANSIENT);

  sqlite3_step (stmt);

  _LOG_DEBUG_COND (sqlite3_errcode (m_db) != SQLITE_OK && sqlite3_errcode (m_db) != SQLITE_ROW, sqlite3_errmsg (m_db));

  // cout << Ccnx::Name (parent_device_name) << endl;

  // assign name to the action, serialize action, and create content object

  sqlite3_finalize (stmt);

  sqlite3_exec (m_db, "END TRANSACTION;", 0,0,0);

  return item;
}


PcoPtr
ActionLog::LookupActionPco (const Ccnx::Name &deviceName, sqlite3_int64 seqno)
{
  sqlite3_stmt *stmt;
  sqlite3_prepare_v2 (m_db, "SELECT action_content_object FROM ActionLog WHERE device_name=? AND seq_no=?", -1, &stmt, 0);

  CcnxCharbufPtr name = deviceName.toCcnxCharbuf ();

  sqlite3_bind_blob  (stmt, 1, name->buf (), name->length (), SQLITE_STATIC);
  sqlite3_bind_int64 (stmt, 2, seqno);

  PcoPtr retval;
  if (sqlite3_step (stmt) == SQLITE_ROW)
    {
      // _LOG_DEBUG (sqlite3_column_blob (stmt, 0) << ", " << sqlite3_column_bytes (stmt, 0));
      retval = make_shared<ParsedContentObject> (reinterpret_cast<const unsigned char *> (sqlite3_column_blob (stmt, 0)), sqlite3_column_bytes (stmt, 0));
    }
  else
    {
      _LOG_TRACE ("No action found for deviceName [" << deviceName << "] and seqno:" << seqno);
    }
  // _LOG_DEBUG_COND (sqlite3_errcode (m_db) != SQLITE_OK && sqlite3_errcode (m_db) != SQLITE_ROW, sqlite3_errmsg (m_db));
  sqlite3_finalize (stmt);

  return retval;
}

ActionItemPtr
ActionLog::LookupAction (const Ccnx::Name &deviceName, sqlite3_int64 seqno)
{
  PcoPtr pco = LookupActionPco (deviceName, seqno);
  if (!pco) return ActionItemPtr ();

  ActionItemPtr action = deserializeMsg<ActionItem> (pco->content ());

  return action;
}

Ccnx::PcoPtr
ActionLog::LookupActionPco (const Ccnx::Name &actionName)
{
  sqlite3_stmt *stmt;
  sqlite3_prepare_v2 (m_db, "SELECT action_content_object FROM ActionLog WHERE action_name=?", -1, &stmt, 0);

  _LOG_DEBUG (actionName);
  CcnxCharbufPtr name = actionName.toCcnxCharbuf ();

  _LOG_DEBUG (" <<<<<<< " << name->buf () << " " << name->length ());

  sqlite3_bind_blob  (stmt, 1, name->buf (), name->length (), SQLITE_STATIC);

  PcoPtr retval;
  if (sqlite3_step (stmt) == SQLITE_ROW)
    {
      // _LOG_DEBUG (sqlite3_column_blob (stmt, 0) << ", " << sqlite3_column_bytes (stmt, 0));
      retval = make_shared<ParsedContentObject> (reinterpret_cast<const unsigned char *> (sqlite3_column_blob (stmt, 0)), sqlite3_column_bytes (stmt, 0));
    }
  else
    {
      _LOG_TRACE ("No action found for name: " << actionName);
    }
  _LOG_DEBUG_COND (sqlite3_errcode (m_db) != SQLITE_OK && sqlite3_errcode (m_db) != SQLITE_ROW, sqlite3_errmsg (m_db));
  sqlite3_finalize (stmt);

  return retval;
}

ActionItemPtr
ActionLog::LookupAction (const Ccnx::Name &actionName)
{
  PcoPtr pco = LookupActionPco (actionName);
  if (!pco) return ActionItemPtr ();

  ActionItemPtr action = deserializeMsg<ActionItem> (pco->content ());

  return action;
}

ActionItemPtr
ActionLog::AddRemoteAction (const Ccnx::Name &deviceName, sqlite3_int64 seqno, Ccnx::PcoPtr actionPco)
{
  if (!actionPco)
    {
      BOOST_THROW_EXCEPTION (Error::ActionLog () << errmsg_info_str ("actionPco is not valid"));
    }
  ActionItemPtr action = deserializeMsg<ActionItem> (actionPco->content ());

  if (!action)
    {
      BOOST_THROW_EXCEPTION (Error::ActionLog () << errmsg_info_str ("action cannot be decoded"));
    }

  sqlite3_stmt *stmt;
  int res = sqlite3_prepare_v2 (m_db, "INSERT INTO ActionLog "
                                "(device_name, seq_no, action, filename, version, action_timestamp, "
                                "file_hash, file_atime, file_mtime, file_ctime, file_chmod, file_seg_num, "
                                "parent_device_name, parent_seq_no, "
                                "action_name, action_content_object) "
                                "VALUES (?, ?, ?, ?, ?, datetime(?, 'unixepoch'),"
                                "        ?, datetime(?, 'unixepoch'), datetime(?, 'unixepoch'), datetime(?, 'unixepoch'), ?,?, "
                                "        ?, ?, "
                                "        ?, ?);", -1, &stmt, 0);

  CcnxCharbufPtr device_name = deviceName.toCcnxCharbuf ();
  sqlite3_bind_blob  (stmt, 1, device_name->buf (), device_name->length (), SQLITE_STATIC);
  sqlite3_bind_int64 (stmt, 2, seqno);

  sqlite3_bind_int   (stmt, 3, action->action ());
  sqlite3_bind_text  (stmt, 4, action->filename ().c_str (), action->filename ().size (), SQLITE_STATIC);
  sqlite3_bind_int64 (stmt, 5, action->version ());
  sqlite3_bind_int64 (stmt, 6, action->timestamp ());

  if (action->action () == ActionItem::UPDATE)
    {
      sqlite3_bind_blob  (stmt, 7, action->file_hash ().c_str (), action->file_hash ().size (), SQLITE_STATIC);

      // sqlite3_bind_int64 (stmt, 8, atime); // NULL
      sqlite3_bind_int64 (stmt, 9, action->mtime ());
      // sqlite3_bind_int64 (stmt, 10, ctime); // NULL

      sqlite3_bind_int   (stmt, 11, action->mode ());
      sqlite3_bind_int   (stmt, 12, action->seg_num ());
    }

  if (action->has_parent_device_name ())
    {
      sqlite3_bind_blob (stmt, 13, action->parent_device_name ().c_str (), action->parent_device_name ().size (), SQLITE_TRANSIENT);
      sqlite3_bind_int64 (stmt, 14, action->parent_seq_no ());
    }

  Name actionName = Name (deviceName)("action")(m_sharedFolderName)(seqno);
  CcnxCharbufPtr namePtr = actionName.toCcnxCharbuf ();

  sqlite3_bind_blob (stmt, 15, namePtr->buf (), namePtr->length (), SQLITE_STATIC);
  sqlite3_bind_blob (stmt, 16, head (actionPco->buf ()), actionPco->buf ().size (), SQLITE_STATIC);
  sqlite3_step (stmt);

  // if action needs to be applied to file state, the trigger will take care of it

  _LOG_DEBUG_COND (sqlite3_errcode (m_db) != SQLITE_OK && sqlite3_errcode (m_db) != SQLITE_ROW, sqlite3_errmsg (m_db));

  sqlite3_finalize (stmt);

  return action;
}

ActionItemPtr
ActionLog::AddRemoteAction (Ccnx::PcoPtr actionPco)
{
  Name name = actionPco->name ();
  // <device_name>/"action"/<shared_folder_name_one_component>/<seqno>

  uint64_t seqno = name.getCompFromBackAsInt (0);
  string sharedFolder = name.getCompFromBackAsString (1);

  if (sharedFolder != m_sharedFolderName)
    {
      BOOST_THROW_EXCEPTION (Error::ActionLog () << errmsg_info_str ("Action doesn't belong to this shared folder"));
    }

  string action = name.getCompFromBackAsString (2);

  if (action != "action")
    {
      BOOST_THROW_EXCEPTION (Error::ActionLog () << errmsg_info_str ("not an action"));
    }
  Name deviceName = name.getPartialName (0, name.size ()-3);

  _LOG_DEBUG ("From [" << name << "] extracted deviceName: " << deviceName << ", sharedFolder: " << sharedFolder << ", seqno: " << seqno);

  return AddRemoteAction (deviceName, seqno, actionPco);
}

sqlite3_int64
ActionLog::LogSize ()
{
  sqlite3_stmt *stmt;
  sqlite3_prepare_v2 (m_db, "SELECT count(*) FROM ActionLog", -1, &stmt, 0);

  sqlite3_int64 retval = -1;
  if (sqlite3_step (stmt) == SQLITE_ROW)
  {
    retval = sqlite3_column_int64 (stmt, 0);
  }

  return retval;
}


///////////////////////////////////////////////////////////////////////////////////
// SHOULD BE MOVED TO SEPARATE FILESTATE CLASS "EVENTUALLY"
///////////////////////////////////////////////////////////////////////////////////

void
ActionLog::apply_action_xFun (sqlite3_context *context, int argc, sqlite3_value **argv)
{
  ActionLog *the = reinterpret_cast<ActionLog*> (sqlite3_user_data (context));

  if (argc != 10)
    {
      sqlite3_result_error (context, "``apply_action'' expects 10 arguments", -1);
      return;
    }

  CcnxCharbuf device_name (sqlite3_value_blob (argv[0]), sqlite3_value_bytes (argv[0]));
  sqlite3_int64 seq_no    = sqlite3_value_int64 (argv[1]);
  int action         = sqlite3_value_int  (argv[2]);
  string filename    = reinterpret_cast<const char*> (sqlite3_value_text (argv[3]));

  _LOG_TRACE ("apply_function called with " << argc);
  _LOG_TRACE ("device_name: " << Name (device_name)
              << ", action: " << action
              << ", file: " << filename);

  if (action == 0) // update
    {
      Hash hash (sqlite3_value_blob (argv[4]), sqlite3_value_bytes (argv[4]));
      time_t atime = static_cast<time_t> (sqlite3_value_int64 (argv[5]));
      time_t mtime = static_cast<time_t> (sqlite3_value_int64 (argv[6]));
      time_t ctime = static_cast<time_t> (sqlite3_value_int64 (argv[7]));
      int mode = sqlite3_value_int (argv[8]);
      int seg_num = sqlite3_value_int (argv[9]);

      _LOG_DEBUG ("Update " << filename << " " << atime << " " << mtime << " " << ctime << " " << hash);

      sqlite3_stmt *stmt;
      sqlite3_prepare_v2 (the->m_db, "UPDATE FileState "
                          "SET "
                          "device_name=?, seq_no=?, "
                          "file_hash=?,"
                          "file_atime=datetime(?, 'unixepoch'),"
                          "file_mtime=datetime(?, 'unixepoch'),"
                          "file_ctime=datetime(?, 'unixepoch'),"
                          "file_chmod=?, "
                          "file_seg_num=? "
                          "WHERE type=0 AND filename=?", -1, &stmt, 0);

      sqlite3_bind_blob  (stmt, 1, device_name.buf (), device_name.length (), SQLITE_TRANSIENT);
      sqlite3_bind_int64 (stmt, 2, seq_no);
      sqlite3_bind_blob  (stmt, 3, hash.GetHash (), hash.GetHashBytes (), SQLITE_TRANSIENT);
      sqlite3_bind_int64 (stmt, 4, atime);
      sqlite3_bind_int64 (stmt, 5, mtime);
      sqlite3_bind_int64 (stmt, 6, ctime);
      sqlite3_bind_int   (stmt, 7, mode);
      sqlite3_bind_int   (stmt, 8, seg_num);
      sqlite3_bind_text  (stmt, 9, filename.c_str (), -1, SQLITE_TRANSIENT);

      sqlite3_step (stmt);

      _LOG_DEBUG_COND (sqlite3_errcode (the->m_db) != SQLITE_OK,
                       sqlite3_errmsg (the->m_db));

      sqlite3_finalize (stmt);

      int affected_rows = sqlite3_changes (the->m_db);
      if (affected_rows == 0) // file didn't exist
        {
          sqlite3_stmt *stmt;
          sqlite3_prepare_v2 (the->m_db, "INSERT INTO FileState "
                              "(type,filename,device_name,seq_no,file_hash,file_atime,file_mtime,file_ctime,file_chmod,file_seg_num) "
                              "VALUES (0, ?, ?, ?, ?, "
                              "datetime(?, 'unixepoch'), datetime(?, 'unixepoch'), datetime(?, 'unixepoch'), ?, ?)", -1, &stmt, 0);

          sqlite3_bind_text  (stmt, 1, filename.c_str (), -1, SQLITE_TRANSIENT);
          sqlite3_bind_blob  (stmt, 2, device_name.buf (), device_name.length (), SQLITE_TRANSIENT);
          sqlite3_bind_int64 (stmt, 3, seq_no);
          sqlite3_bind_blob  (stmt, 4, hash.GetHash (), hash.GetHashBytes (), SQLITE_TRANSIENT);
          sqlite3_bind_int64 (stmt, 5, atime);
          sqlite3_bind_int64 (stmt, 6, mtime);
          sqlite3_bind_int64 (stmt, 7, ctime);
          sqlite3_bind_int   (stmt, 8, mode);
          sqlite3_bind_int   (stmt, 9, seg_num);

          sqlite3_step (stmt);
          _LOG_DEBUG_COND (sqlite3_errcode (the->m_db) != SQLITE_OK,
                           sqlite3_errmsg (the->m_db));
          sqlite3_finalize (stmt);
        }
    }
  else if (action == 1) // delete
    {
      sqlite3_stmt *stmt;
      sqlite3_prepare_v2 (the->m_db, "DELETE FROM FileState WHERE type=0 AND filename=?", -1, &stmt, 0);
      sqlite3_bind_text (stmt, 1, filename.c_str (), -1, SQLITE_STATIC);

      _LOG_DEBUG ("Delete " << filename);

      sqlite3_step (stmt);
      sqlite3_finalize (stmt);

      the->m_onFileRemoved (filename);
    }

  sqlite3_result_null (context);
}

/**
 * @todo Implement checking modification time and permissions
 */
FileItemPtr
ActionLog::LookupFile (const std::string &filename)
{
  sqlite3_stmt *stmt;
  sqlite3_prepare_v2 (m_db,
                      "SELECT filename,device_name,seq_no,file_hash,strftime('%s', file_mtime),file_chmod,file_seg_num "
                      "       FROM FileState "
                      "       WHERE type = 0 AND filename = ?", -1, &stmt, 0);
  sqlite3_bind_text(stmt, 1, filename.c_str(), -1, SQLITE_STATIC);

  FileItemPtr retval;
  if (sqlite3_step (stmt) == SQLITE_ROW)
  {
    retval = make_shared<FileItem> ();
    retval->set_filename    (reinterpret_cast<const char *> (sqlite3_column_text  (stmt, 0)), sqlite3_column_bytes (stmt, 0));
    retval->set_device_name (sqlite3_column_blob  (stmt, 1), sqlite3_column_bytes (stmt, 1));
    retval->set_seq_no      (sqlite3_column_int64 (stmt, 2));
    retval->set_file_hash   (sqlite3_column_blob  (stmt, 3), sqlite3_column_bytes (stmt, 3));
    retval->set_mtime       (sqlite3_column_int   (stmt, 4));
    retval->set_mode        (sqlite3_column_int   (stmt, 5));
    retval->set_seg_num     (sqlite3_column_int64 (stmt, 6));
  }
  sqlite3_finalize (stmt);

  return retval;
}

FileItemsPtr
ActionLog::LookupFilesForHash (const Hash &hash)
{
  sqlite3_stmt *stmt;
  sqlite3_prepare_v2 (m_db,
                      "SELECT filename,device_name,seq_no,file_hash,strftime('%s', file_mtime),file_chmod,file_seg_num "
                      "   FROM FileState "
                      "   WHERE type = 0 AND file_hash = ?;", -1, &stmt, 0);
  sqlite3_bind_blob(stmt, 1, hash.GetHash (), hash.GetHashBytes (), SQLITE_STATIC);

  FileItemsPtr retval = make_shared<FileItems> ();
  while (sqlite3_step (stmt) == SQLITE_ROW)
    {
      FileItem file;
      file.set_filename    (reinterpret_cast<const char *> (sqlite3_column_text  (stmt, 0)), sqlite3_column_bytes (stmt, 0));
      file.set_device_name (sqlite3_column_blob  (stmt, 1), sqlite3_column_bytes (stmt, 1));
      file.set_seq_no      (sqlite3_column_int64 (stmt, 2));
      file.set_file_hash   (sqlite3_column_blob  (stmt, 3), sqlite3_column_bytes (stmt, 3));
      file.set_mtime       (sqlite3_column_int   (stmt, 4));
      file.set_mode        (sqlite3_column_int   (stmt, 5));
      file.set_seg_num     (sqlite3_column_int64 (stmt, 6));

      retval->push_back (file);
    }

  sqlite3_finalize (stmt);

  return retval;
}
