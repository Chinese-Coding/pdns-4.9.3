/*  SQLite backend for PowerDNS
 *  Copyright (C) 2003, Michel Stol <michel@powerdns.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2
 *  as published by the Free Software Foundation.
 *
 *  Additionally, the license of this program contains a special
 *  exception which allows to distribute the program in binary form when
 *  it is linked against OpenSSL.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <string>
#include <sstream>
#include "ssqlite3.hh"
#include <iostream>
#include <fstream>
#include <utility>
#include "pdns/logger.hh"
#include "misc.hh"
#include "utility.hh"
#include <unistd.h>

/*
** Set all the parameters in the compiled SQL statement to NULL.
*
* copied from sqlite 3.3.6 // cmouse
*/
#if SQLITE_VERSION_NUMBER < 3003009
static int pdns_sqlite3_clear_bindings(sqlite3_stmt* pStmt)
{
  int i;
  int rc = SQLITE_OK;
  for (i = 1; rc == SQLITE_OK && i <= sqlite3_bind_parameter_count(pStmt); i++) {
    rc = sqlite3_bind_null(pStmt, i);
  }
  return rc;
}
#endif

static string SSQLite3ErrorString(sqlite3* database)
{
  return string(sqlite3_errmsg(database) + string(" (") + std::to_string(sqlite3_extended_errcode(database)) + string(")"));
}

class SSQLite3Statement : public SSqlStatement
{
public:
  SSQLite3Statement(SSQLite3* database, bool dolog, string query) :
    d_query(std::move(query)),
    d_db(database),
    d_dolog(dolog)
  {
  }

  SSQLite3Statement(const SSQLite3Statement&) = delete;
  SSQLite3Statement(SSQLite3Statement&&) = delete;
  SSQLite3Statement& operator=(const SSQLite3Statement&) = delete;
  SSQLite3Statement& operator=(SSQLite3Statement&&) = delete;

  int name2idx(const string& name)
  {
    string zName = string(":") + name;
    prepareStatement();
    return sqlite3_bind_parameter_index(d_stmt, zName.c_str());
    // XXX: support @ and $?
  }

  SSqlStatement* bind(const string& name, bool value) override
  {
    int idx = name2idx(name);
    if (idx > 0) {
      sqlite3_bind_int(d_stmt, idx, value ? 1 : 0);
    };
    return this;
  }

  SSqlStatement* bind(const string& name, int value) override
  {
    int idx = name2idx(name);
    if (idx > 0) {
      sqlite3_bind_int(d_stmt, idx, value);
    };
    return this;
  }

  SSqlStatement* bind(const string& name, uint32_t value) override
  {
    int idx = name2idx(name);
    if (idx > 0) {
      sqlite3_bind_int64(d_stmt, idx, value);
    };
    return this;
  }

  SSqlStatement* bind(const string& name, long value) override
  {
    int idx = name2idx(name);
    if (idx > 0) {
      sqlite3_bind_int64(d_stmt, idx, value);
    };
    return this;
  }

  SSqlStatement* bind(const string& name, unsigned long value) override
  {
    int idx = name2idx(name);
    if (idx > 0) {
      sqlite3_bind_int64(d_stmt, idx, static_cast<sqlite3_int64>(value));
    };
    return this;
  }

  SSqlStatement* bind(const string& name, long long value) override
  {
    int idx = name2idx(name);
    if (idx > 0) {
      sqlite3_bind_int64(d_stmt, idx, value);
    };
    return this;
  };

  SSqlStatement* bind(const string& name, unsigned long long value) override
  {
    int idx = name2idx(name);
    if (idx > 0) {
      sqlite3_bind_int64(d_stmt, idx, static_cast<sqlite3_int64>(value));
    };
    return this;
  }

  SSqlStatement* bind(const string& name, const std::string& value) override
  {
    int idx = name2idx(name);
    if (idx > 0) {
      sqlite3_bind_text(d_stmt, idx, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
    };
    return this;
  }

  SSqlStatement* bindNull(const string& name) override
  {
    int idx = name2idx(name);
    if (idx > 0) {
      sqlite3_bind_null(d_stmt, idx);
    };
    return this;
  }

  SSqlStatement* execute() override
  {
    prepareStatement();
    if (d_dolog) {
      g_log << Logger::Warning << "Query " << this << ": " << d_query << endl;
      d_dtime.set();
    }

    int attempts = d_db->inTransaction() ? 1 : 0; // try only once
    while (attempts < 2 && (d_rc = sqlite3_step(d_stmt)) == SQLITE_BUSY) {
      attempts++;
    }

    if (d_rc != SQLITE_ROW && d_rc != SQLITE_DONE) {
      // failed.
      releaseStatement();
      if (d_rc == SQLITE_CANTOPEN) {
        throw SSqlException(string("CANTOPEN error in sqlite3, often caused by unwritable sqlite3 db *directory*: ") + SSQLite3ErrorString(d_db->db()));
      }
      throw SSqlException(string("Error while retrieving SQLite query results: ") + SSQLite3ErrorString(d_db->db()));
    }
    if (d_dolog) {
      g_log << Logger::Warning << "Query " << this << ": " << d_dtime.udiffNoReset() << " us to execute" << endl;
    }
    return this;
  }

  bool hasNextRow() override
  {
    if (d_dolog && d_rc != SQLITE_ROW) {
      g_log << Logger::Warning << "Query " << this << ": " << d_dtime.udiffNoReset() << " us total to last row" << endl;
    }
    return d_rc == SQLITE_ROW;
  }

  SSqlStatement* nextRow(row_t& row) override
  {
    row.clear();
    int numCols = sqlite3_column_count(d_stmt);
    row.reserve(numCols); // preallocate memory
    // Another row received, process it.
    for (int i = 0; i < numCols; i++) {
      if (sqlite3_column_type(d_stmt, i) == SQLITE_NULL) {
        row.emplace_back("");
      }
      else {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast): SQLite API returns unsigned char strings and std::strings are signed char.
        const char* pData = (const char*)sqlite3_column_text(d_stmt, i);
        row.emplace_back(pData, sqlite3_column_bytes(d_stmt, i));
      }
    }
    d_rc = sqlite3_step(d_stmt);
    return this;
  }

  SSqlStatement* getResult(result_t& result) override
  {
    result.clear();
    while (hasNextRow()) {
      row_t row;
      nextRow(row);
      result.push_back(std::move(row));
    }
    return this;
  }

  SSqlStatement* reset() override
  {
    sqlite3_reset(d_stmt);
#if SQLITE_VERSION_NUMBER >= 3003009
    sqlite3_clear_bindings(d_stmt);
#else
    pdns_sqlite3_clear_bindings(d_stmt);
#endif
    return this;
  }

  ~SSQLite3Statement() override
  {
    // deallocate if necessary
    releaseStatement();
  }

  const string& getQuery() override { return d_query; };

private:
  string d_query;
  DTime d_dtime;
  sqlite3_stmt* d_stmt{nullptr};
  SSQLite3* d_db{nullptr};
  int d_rc{0};
  bool d_dolog;
  bool d_prepared{false};

  void prepareStatement()
  {
    const char* pTail = nullptr;

    if (d_prepared) {
      return;
    }
#if SQLITE_VERSION_NUMBER >= 3003009
    if (sqlite3_prepare_v2(d_db->db(), d_query.c_str(), -1, &d_stmt, &pTail) != SQLITE_OK)
#else
    if (sqlite3_prepare(d_db->db(), d_query.c_str(), -1, &d_stmt, &pTail) != SQLITE_OK)
#endif
    {
      releaseStatement();
      throw SSqlException(string("Unable to compile SQLite statement : '") + d_query + "': " + SSQLite3ErrorString(d_db->db()));
    }
    if ((pTail != nullptr) && strlen(pTail) > 0) {
      g_log << Logger::Warning << "Sqlite3 command partially processed. Unprocessed part: " << pTail << endl;
    }
    d_prepared = true;
  }

  void releaseStatement()
  {
    if (d_stmt != nullptr) {
      sqlite3_finalize(d_stmt);
    }
    d_stmt = nullptr;
    d_prepared = false;
  }
};

// Constructor.
SSQLite3::SSQLite3(const std::string& database, const std::string& journalmode, bool creat)
{
  if (access(database.c_str(), F_OK) == -1) {
    if (!creat) {
      throw SSqlException("SQLite database '" + database + "' does not exist yet");
    }
  }
  else {
    if (creat) {
      throw SSqlException("SQLite database '" + database + "' already exists");
    }
  }

  if (sqlite3_open(database.c_str(), &m_pDB) != SQLITE_OK) {
    throw SSqlException("Could not connect to the SQLite database '" + database + "'");
  }
  m_dolog = false;
  m_in_transaction = false;
  sqlite3_busy_handler(m_pDB, busyHandler, nullptr);

  if (journalmode.length() != 0) {
    executeImpl("PRAGMA journal_mode=" + journalmode);
  }
}

void SSQLite3::setLog(bool state)
{
  m_dolog = state;
}

// Destructor.
SSQLite3::~SSQLite3()
{
  for (int tries = 0;; ++tries) {
    int ret = sqlite3_close(m_pDB);
    if (ret != SQLITE_OK) {
      if (tries != 0 || ret != SQLITE_BUSY) { // if we have SQLITE_BUSY, and a working m_Pstmt, try finalize
        cerr << "Unable to close down sqlite connection: " << ret << endl;
        abort();
      }
    }
    else {
      break;
    }
  }
}

std::unique_ptr<SSqlStatement> SSQLite3::prepare(const string& query, int nparams __attribute__((unused)))
{
  return std::make_unique<SSQLite3Statement>(this, m_dolog, query);
}

void SSQLite3::executeImpl(const string& query)
{
  char* errmsg = nullptr;
  std::string errstr1;
  int execRet = sqlite3_exec(m_pDB, query.c_str(), nullptr, nullptr, &errmsg);
  if (execRet != SQLITE_OK) {
    errstr1 = errmsg;
    sqlite3_free(errmsg);
  }
  if (execRet == SQLITE_BUSY) {
    if (m_in_transaction) {
      throw SSqlException("Failed to execute query: " + errstr1);
    }
    execRet = sqlite3_exec(m_pDB, query.c_str(), nullptr, nullptr, &errmsg);
    std::string errstr2;
    if (execRet != SQLITE_OK) {
      errstr2 = errmsg;
      sqlite3_free(errmsg);
      throw SSqlException("Failed to execute query: " + errstr2);
    }
  }
  else if (execRet != SQLITE_OK) {
    throw SSqlException("Failed to execute query: " + errstr1);
  }
}

void SSQLite3::execute(const string& query)
{
  executeImpl(query);
}

int SSQLite3::busyHandler([[maybe_unused]] void* userData, [[maybe_unused]] int invocationsSoFar)
{
  Utility::usleep(1000);
  return 1;
}

void SSQLite3::startTransaction()
{
  execute("begin immediate");
  m_in_transaction = true;
}

void SSQLite3::rollback()
{
  execute("rollback");
  m_in_transaction = false;
}

void SSQLite3::commit()
{
  execute("commit");
  m_in_transaction = false;
}

// Constructs a SSqlException object.
SSqlException SSQLite3::sPerrorException(const std::string& reason)
{
  return {reason};
}
