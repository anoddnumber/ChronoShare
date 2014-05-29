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

#include "content-server.h"
#include "logging.h"
#include <boost/make_shared.hpp>
#include <utility>
#include "task.h"
#include "periodic-task.h"
#include "simple-interval-generator.h"
#include <boost/lexical_cast.hpp>

INIT_LOGGER ("ContentServer");

using namespace ndn;
using namespace std;
using namespace boost;

static const int DB_CACHE_LIFETIME = 60;

ContentServer::ContentServer(ndn::Face face, ActionLogPtr actionLog,
                             const boost::filesystem::path &rootDir,
                             const ndn::Name &userName, const std::string &sharedFolderName,
                             const std::string &appName,
                             int freshness)
  : m_ndn(face)
  , m_actionLog(actionLog)
  , m_dbFolder(rootDir / ".chronoshare")
  , m_freshness(freshness)
  , m_scheduler (new Scheduler())
  , m_userName (userName)
  , m_sharedFolderName (sharedFolderName)
  , m_appName (appName)
{
  m_scheduler->start ();
  TaskPtr flushStaleDbCacheTask = boost::make_shared<PeriodicTask>(boost::bind(&ContentServer::flushStaleDbCache, this), "flush-state-db-cache", m_scheduler, boost::make_shared<SimpleIntervalGenerator>(DB_CACHE_LIFETIME));
  m_scheduler->addTask(flushStaleDbCacheTask);
}

ContentServer::~ContentServer()
{
  m_scheduler->shutdown ();

  ScopedLock lock (m_mutex);
  for (PrefixIt forwardingHint = m_prefixes.begin(); forwardingHint != m_prefixes.end(); ++forwardingHint) //TODO does ++forwardingHint work still?
  {
    m_ndn.unsetInterestFilter(*forwardingHint);
  }

  m_prefixes.clear ();
}

void
ContentServer::registerPrefix (const ndn::Name &forwardingHint)
{
  // Format for files:   /<forwarding-hint>/<device_name>/<appname>/file/<hash>/<segment>
  // Format for actions: /<forwarding-hint>/<device_name>/<appname>/action/<shared-folder>/<action-seq>

  _LOG_DEBUG (">> content server: register " << forwardingHint);

  RegisteredPrefixId id = m_ndn.setInterestFilter (forwardingHint, bind(&ContentServer::filterAndServe, this, forwardingHint, _1), bind(&ContentServer::doNothing, this));

  ScopedLock lock (m_mutex);
  m_prefixes.insert(id);
}

void
ContentServer::deregisterPrefix (const RegisteredPrefixId &forwardingHint)
{
  _LOG_DEBUG ("<< content server: deregister " << forwardingHint);
  m_ndn.unsetInterestFilter(forwardingHint);

  ScopedLock lock (m_mutex);
  m_prefixes.erase (forwardingHint);
}


void
ContentServer::filterAndServeImpl (const ndn::Name &forwardingHint, const Name &name, const ndn::Name &interest)
{
  // interest for files:   /<forwarding-hint>/<device_name>/<appname>/file/<hash>/<segment>
  // interest for actions: /<forwarding-hint>/<device_name>/<appname>/action/<shared-folder>/<action-seq>

  // name for files:   /<device_name>/<appname>/file/<hash>/<segment>
  // name for actions: /<device_name>/<appname>/action/<shared-folder>/<action-seq>

  try
    {
      if (name.size() >= 4 && name.getCompFromBackAsString (3) == m_appName)
        {
          string type = name.getCompFromBackAsString (2);
          if (type == "file")
            {
              serve_File (forwardingHint, name, interest);
            }
          else if (type == "action")
            {
              string folder = name.getCompFromBackAsString (1);
              if (folder == m_sharedFolderName)
              {
                serve_Action (forwardingHint, name, interest);
              }
            }
        }
    }
  catch (Ccnx::NameException &ne) //TODO Ccnx::NameException
    {
      // ignore any unexpected interests and errors
      _LOG_ERROR(boost::get_error_info<Ccnx::error_info_str>(ne));
    }
}

void
ContentServer::doNothing ()
{

}

void
ContentServer::filterAndServe (ndn::Name forwardingHint, const ndn::Name &interest)
{
  try
    {
      if (forwardingHint.size () > 0 &&
          m_userName.size () >= forwardingHint.size () &&
          m_userName.getSubName (0, forwardingHint.size ()) == forwardingHint)
        {
          filterAndServeImpl (Name ("/"), interest, interest); // try without forwarding hints
        }

      filterAndServeImpl (forwardingHint, interest.getSubName (forwardingHint.size()), interest); // always try with hint... :( have to
    }
  catch (Ccnx::NameException &ne)
    {
      // ignore any unexpected interests and errors
      _LOG_ERROR(boost::get_error_info<Ccnx::error_info_str>(ne));
    }
}

void
ContentServer::serve_Action (const ndn::Name &forwardingHint, const ndn::Name &name, const ndn::Name &interest)
{
  _LOG_DEBUG (">> content server serving ACTION, hint: " << forwardingHint << ", interest: " << interest);
  m_scheduler->scheduleOneTimeTask (m_scheduler, 0, bind (&ContentServer::serve_Action_Execute, this, forwardingHint, name, interest), boost::lexical_cast<string>(name));
  // need to unlock ccnx mutex... or at least don't lock it
}

void
ContentServer::serve_File (const ndn::Name &forwardingHint, const ndn::Name &name, const ndn::Name &interest)
{
  _LOG_DEBUG (">> content server serving FILE, hint: " << forwardingHint << ", interest: " << interest);

  m_scheduler->scheduleOneTimeTask (m_scheduler, 0, bind (&ContentServer::serve_File_Execute, this, forwardingHint, name, interest), boost::lexical_cast<string>(name));
  // need to unlock ccnx mutex... or at least don't lock it
}

void
ContentServer::serve_File_Execute (const ndn::Name &forwardingHint, const ndn::Name &name, const ndn::Name &interest)
{
  // forwardingHint: /<forwarding-hint>
  // interest:       /<forwarding-hint>/<device_name>/<appname>/file/<hash>/<segment>
  // name:           /<device_name>/<appname>/file/<hash>/<segment>

  int64_t segment = name.getCompFromBackAsInt (0);
  ndn::Name deviceName = name.getPartialName (0, name.size () - 4);
  Hash hash (head(name.getCompFromBack (1)), name.getCompFromBack (1).size());

  _LOG_DEBUG (" server FILE for device: " << deviceName << ", file_hash: " << hash.shortHash () << " segment: " << segment);

  string hashStr = lexical_cast<string> (hash);

  ObjectDbPtr db;

  ScopedLock(m_dbCacheMutex);
  {
    DbCache::iterator it = m_dbCache.find(hash);
    if (it != m_dbCache.end())
    {
      db = it->second;
    }
    else
    {
      if (ObjectDb::DoesExist (m_dbFolder, deviceName, hashStr)) // this is kind of overkill, as it counts available segments
        {
         db = boost::make_shared<ObjectDb>(m_dbFolder, hashStr);
         m_dbCache.insert(make_pair(hash, db));
        }
      else
        {
          _LOG_ERROR ("ObjectDd doesn't exist for device: " << deviceName << ", file_hash: " << hash.shortHash ());
        }
    }
  }

  if (db)
  {
	ndn::Buffer co = db->fetchSegment (deviceName, segment);
    if (co)
      {
        if (forwardingHint.size () == 0)
          {
            _LOG_DEBUG (ParsedContentObject (*co).name ());
            ndn::Data data;
            data.setContent(co->buf (), co->size ());
            m_ndn.put(data);
          }
        else
          {
            if (m_freshness > 0)
              {
                ndn::Data data;
                data.setName(interest);
                data.setFreshnessPeriod(m_freshness);
                data.setContent(co->buf (), co->size ());
                m_ndn.put(data);
              }
            else
              {
                ndn::Data data;
                data.setName(interest);
                data.setContent(co->buf (), co->size ());
                m_ndn->put(data);
              }
          }

      }
    else
      {
        _LOG_ERROR ("ObjectDd exists, but no segment " << segment << " for device: " << deviceName << ", file_hash: " << hash.shortHash ());
      }

  }
}

void
ContentServer::serve_Action_Execute (const ndn::Name &forwardingHint, const ndn::Name &name, const ndn::Name &interest)
{
  // forwardingHint: /<forwarding-hint>
  // interest:       /<forwarding-hint>/<device_name>/<appname>/action/<shared-folder>/<action-seq>
  // name for actions: /<device_name>/<appname>/action/<shared-folder>/<action-seq>

  int64_t seqno = name.getCompFromBackAsInt (0);
  ndn::Name deviceName = name.getPartialName (0, name.size () - 4);

  _LOG_DEBUG (" server ACTION for device: " << deviceName << " and seqno: " << seqno);
//DATA
  shared_ptr<Data> dataObject = m_actionLog->LookupActionData (deviceName, seqno);
  if (dataObject)
    {
      if (forwardingHint.size () == 0)
        {
          m_ndn.put (dataObject);
        }
      else
        {
          const Block &block = dataObject->getContent ();
          const ndn::Buffer &content = block->value_begin (); //TODO: ...is this correct...?

          if (m_freshness > 0)
            {
              ndn::Data data;
			  data.setName(interest);
			  data.setFreshnessPeriod(m_freshness);
			  data.setContent(content.buf (), content.size());
			  m_ndn.put(data);
            }
          else
            {
              ndn::Data data;
              data.setName(interest);
              data.setContent(content.buf (), content.size());
              m_ndn.put(data);
            }
        }
    }
  else
    {
      _LOG_ERROR ("ACTION not found for device: " << deviceName << " and seqno: " << seqno);
    }
}

void
ContentServer::flushStaleDbCache()
{
  ScopedLock(m_dbCacheMutex);
  DbCache::iterator it = m_dbCache.begin();
  while (it != m_dbCache.end())
  {
    ObjectDbPtr db = it->second;
    if (db->secondsSinceLastUse() >= DB_CACHE_LIFETIME)
    {
      m_dbCache.erase(it++);
    }
    else
    {
      ++it;
    }
  }
}
