/*
   Copyright (c) 2015, 2016, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

#include "srv_session.h"
#include "my_dbug.h"
#include "sql_class.h"
#include "sql_base.h"            // close_mysql_tables
#include "sql_connect.h"         // thd_init_client_charset
#include "sql_audit.h"           // MYSQL_AUDIT_NOTIFY_CONNECTION_CONNECT
#include "log.h"                 // Query log
#include "my_pthread.h"           // pthread_equal()
#include "mysqld.h"              // current_thd
#include "sql_parse.h"           // dispatch_command()
#include "sql_acl.h"             // acl_getroot()
#include "mysql/thread_pool_priv.h" // thd_set_thread_stack
#include "mysql/psi/psi.h"
#include "mysql/psi/mysql_thread.h"
//#include "conn_handler/connection_handler_manager.h"
#include "sql_plugin.h"
#include "my_stacktrace.h"

#include <map>

/**
  @file
  class Srv_session implementation. See the method comments for more. Please,
  check also srv_session.h for more information.
*/


extern void thd_clear_errors(THD *thd);

static bool srv_session_THRs_initialized= false;

/**
  A simple wrapper around a RW lock:
  Grabs the lock in the CTOR, releases it in the DTOR.
  The lock may be NULL, in which case this is a no-op.

*/
class Auto_rw_lock_read
{
public:
  explicit Auto_rw_lock_read(mysql_rwlock_t *lock) : rw_lock(NULL)
  {
    if (lock && 0 == mysql_rwlock_rdlock(lock))
      rw_lock = lock;
    else {
      my_safe_printf_stderr("Failed to initialize Auto_rw_lock_read");
      abort();
    }
  }

  ~Auto_rw_lock_read()
  {
    if (rw_lock)
      mysql_rwlock_unlock(rw_lock);
  }
private:
  mysql_rwlock_t *rw_lock;

  Auto_rw_lock_read(const Auto_rw_lock_read&);         /* Not copyable. */
  void operator=(const Auto_rw_lock_read&);            /* Not assignable. */
};


class Auto_rw_lock_write
{
public:
  explicit Auto_rw_lock_write(mysql_rwlock_t *lock) : rw_lock(NULL)
  {
    if (lock && 0 == mysql_rwlock_wrlock(lock))
      rw_lock = lock;
    else {
      my_safe_printf_stderr("Failed to initialize Auto_rw_lock_write");
      abort();
    }
  }

  ~Auto_rw_lock_write()
  {
    if (rw_lock)
      mysql_rwlock_unlock(rw_lock);
  }
private:
  mysql_rwlock_t *rw_lock;

  Auto_rw_lock_write(const Auto_rw_lock_write&);        /* Non-copyable */
  void operator=(const Auto_rw_lock_write&);            /* Non-assignable */
};

/**
 std::map of THD* as key and Srv_session* as value guarded by a read-write lock.
 RW lock is used instead of a mutex, as find() is a hot spot due to the sanity
 checks it is used for - when a pointer to a closed session is passed.
*/
class Mutexed_map_thd_srv_session
{
public:
  class Do_Impl
  {
  public:
    virtual ~Do_Impl() {}
    /**
      Work on the session

      @return
        false  Leave the session in the map
        true   Remove the session from the map
    */
    virtual bool operator()(Srv_session*) = 0;
  };

private:
  typedef my_thread_id map_key_t;
  typedef std::shared_ptr<Srv_session> map_value_t;

  std::unordered_map<map_key_t, map_value_t> collection;

  std::atomic_bool initted;

  mysql_rwlock_t LOCK_collection;

#ifdef HAVE_PSI_INTERFACE
  PSI_rwlock_key key_LOCK_collection;
#endif

public:
  /**
    Initializes the map

    @param null_val null value to be returned when element not found in the map
  */
  void init()
  {
    initted.store(true);

#ifdef HAVE_PSI_INTERFACE
    PSI_rwlock_info all_rwlocks[]=
    {
      { &key_LOCK_collection, "LOCK_srv_session_collection", PSI_FLAG_GLOBAL}
    };

    mysql_rwlock_register("session", all_rwlocks, array_elements(all_rwlocks));
#endif
    mysql_rwlock_init(key_LOCK_collection, &LOCK_collection);
  }

  /**
    Searches for an element with in the map

    @param key Key of the element

    @return
      value of the element
      NULL  if not found
  */
  map_value_t find(const map_key_t& key)
  {
    if (!initted.load()) // if map already destroyed
      return NULL;

    Auto_rw_lock_read lock(&LOCK_collection);

    auto it= collection.find(key);
    if (it == collection.end()) {
      return nullptr;
    }
    return it->second;
  }

  /**
    Add an element to the map

    @param key     key
    @param value   value

    @return
      false  success
      true   failure
  */
  bool add(const map_key_t& key, map_value_t session)
  {
    if (!initted.load()) // if map already destroyed
      return true;

    Auto_rw_lock_write lock(&LOCK_collection);
    try
    {
      auto it = collection.find(key);
      if (it != collection.end() && it->second) {
        DBUG_PRINT("error", ("Session with id %d already exists.", key));
        return true;
      }
      collection[key]= std::move(session);
      DBUG_PRINT("info", ("Stored session in map, sid=%d", key));
    }
    catch (const std::bad_alloc &e)
    {
      return true;
    }
    return false;
  }

  /**
    Removes an element from the map.

    @param key  key
  */
  void remove(const map_key_t& key)
  {
    if (!initted.load()) // if map already destroyed
      return;

    Auto_rw_lock_write lock(&LOCK_collection);
    /*
      If we use erase with the key directly an exception could be thrown. The
      find method never throws. erase() with iterator as parameter also never
      throws.
    */
    auto it= collection.find(key);
    if (it != collection.end())
    {
      DBUG_PRINT("info", ("Removed srv session from map %d", key));
      collection.erase(it);
    }
  }

  /**
    Empties the map
  */
  void deinit()
  {
    initted.store(false);

    collection.clear();

    mysql_rwlock_destroy(&LOCK_collection);
  }

  /**
    Returns the number of elements in the maps
  */
  unsigned int size()
  {
    Auto_rw_lock_read lock(&LOCK_collection);
    return collection.size();
  }

  /**
    Returns a copy of the sessions sorted by thread id
  */
  std::vector<map_value_t> get_sorted_srv_session_list()
  {
    std::vector<map_value_t> session_list;
    {
      Auto_rw_lock_read lock(&LOCK_collection);

      for (const auto& it: collection) {
        DBUG_PRINT("info", ("session id %u", it.second->get_session_id()));
        session_list.push_back(it.second);
      }
    }
    std::sort(session_list.begin(), session_list.end(),
        [](map_value_t s1, map_value_t s2) {
        return s1->get_session_id() < s2->get_session_id();
      });
    return session_list;
  }
};

static Mutexed_map_thd_srv_session server_session_list;

std::shared_ptr<Srv_session> Srv_session::find_session(
    const std::string& string_key) {
  char *endptr = 0;
  const char* string_key_cstr = string_key.c_str();

  if (string_key.size() > MAX_INT_WIDTH) {
    DBUG_PRINT("error", ("Wrong format for rpc_id '%s'", string_key_cstr));
    return nullptr;
  }

  errno = 0;    /* To distinguish success/failure after call */
  long int session_id = strtol(string_key_cstr, &endptr, 10);

  /* Check for various possible errors */
  if (errno != 0 || endptr == 0 || *endptr != '\0' || session_id <= 0) {
    DBUG_PRINT("error", ("Wrong format for rpc_id '%s'", string_key_cstr));
    return nullptr;
  }

  return find_session(session_id);
}

std::shared_ptr<Srv_session> Srv_session::find_session(
    my_thread_id session_id) {
  return server_session_list.find(session_id);
}

void Srv_session::remove_session(my_thread_id session_id) {
  server_session_list.remove(session_id);
}

bool Srv_session::store_session(std::shared_ptr<Srv_session> session) {
  return server_session_list.add(session->get_session_id(), session);
}

std::vector<std::shared_ptr<Srv_session>> Srv_session::get_sorted_sessions() {
  DBUG_PRINT("info", ("sessions list size %d", server_session_list.size()));
  return server_session_list.get_sorted_srv_session_list();
}

/**
  Modifies the PSI structures to (de)install a THD

  @param thd THD
*/
static void set_psi(THD *thd)
{
#ifdef HAVE_PSI_THREAD_INTERFACE
  struct PSI_thread *psi= PSI_THREAD_CALL(get_thread)();
  PSI_THREAD_CALL(set_thread_id)(psi, thd? thd->thread_id() : 0);
#endif
}

/**
  Inits the module

  @return
    false  success
    true   failure
*/
bool Srv_session::module_init()
{
  if (srv_session_THRs_initialized)
    return false;

  srv_session_THRs_initialized= true;

  server_session_list.init();

  return false;
}


/**
  Deinits the module.

  Never fails

  @return
    false  success
*/
bool Srv_session::module_deinit()
{
  DBUG_ENTER("Srv_session::module_deinit");
  if (srv_session_THRs_initialized)
  {
    server_session_list.deinit();

    srv_session_THRs_initialized= false;
  }
  DBUG_RETURN(false);
}


/**
  Constructs a server session

  @param error_cb       Default completion callback
  @param err_cb_ctx     Plugin's context, opaque pointer that would
                        be provided to callbacks. Might be NULL.
*/
Srv_session::Srv_session() : state_(SRV_SESSION_CREATED)
{
  thd_.mark_as_srv_session();
  // needed for Valgrind not to complain of "Conditional jump"
  thd_.net.reading_or_writing= 0;

  default_vio_to_restore_ = thd_.net.vio;
  default_stmt_to_restore_ = thd_.get_stmt_da();
}


/**
  Opens a server session

  @return
    false  on success
    true   on failure
*/

bool Srv_session::open()
{
  DBUG_ENTER("Srv_session::open");

  DBUG_PRINT("info",("Session=%p  THD=%p session_id=%d",
                    this, get_thd(), get_session_id()));

  DBUG_ASSERT(get_state() == SRV_SESSION_CREATED);

  /*
    thd_.stack_start will be set once we start attempt to attach.
    store_globals() will check for it, so we will set it beforehand.

    No store_globals() here as the session is always created in a detached
    state. Attachment with store_globals() will happen on demand.
  */
  if (thd_init_client_charset(get_thd(), my_charset_utf8_general_ci.number))
  {
    connection_errors_internal++;
    DBUG_RETURN(true);
  }

  thd_.update_charset();

  thd_.set_new_thread_id();

  thd_.set_time();
  thd_.thr_create_utime= thd_.start_utime= my_micro_time();

  /*
    Disable QC - plugins will most probably install their own protocol
    and it won't be compatible with the QC. In addition, Protocol_error
    is not compatible with the QC.
  */
  thd_.variables.query_cache_type = 0;

  thd_.set_command(COM_SLEEP);
  thd_.init_for_queries();

  DBUG_RETURN(false);
}

/**
  Try to switch state to attached.

  @param session  Session handle

  @returns
    false   success
    true    failure
*/
bool Srv_session::wait_to_attach() {
  std::unique_lock<std::mutex> lock(mutex_);

  switch(state_) {
  case SRV_SESSION_CREATED:
  case SRV_SESSION_DETACHED:
  {
    switch_state_safe(SRV_SESSION_ATTACHED);
    return false;
  }
  case SRV_SESSION_TO_BE_DETACHED:
  {
    DBUG_PRINT("info", ("State is SRV_SESSION_TO_BE_DETACHED, waiting 100ms"));
    // wait on condition variable with timeout
    auto before = std::chrono::system_clock::now();
    auto timeout = before + std::chrono::microseconds(100);

    if (wait_to_attach_.wait_until(lock, timeout, [this] {
              return state_ == SRV_SESSION_DETACHED; })) {
      switch_state_safe(SRV_SESSION_ATTACHED);
      DBUG_PRINT("info", ("Suceeded to attach session, srv_thd=%p, time=%ldms",
              get_thd(), (std::chrono::system_clock::now() - before).count()));
      return false;
    }
    // erorr, either:
    // - timeout waiting on cond var
    // - another thread attached the session
    // - it's in DESTROY state
    DBUG_PRINT("error", ("Failed to attach session, state=%d, srv thd=%p,"
          " time=%ldms", state_, get_thd(),
          (std::chrono::system_clock::now() - before).count()));
    return true;
  }
  default:
    DBUG_PRINT("error", ("Tried to attach session from unexpected state=%d, "
          "srv thd=%p, attached to conn_tid=%d", state_, get_thd(),
          get_conn_thd_id()));
    return true;
  }
  // should never get here
  return true;
}

/**
  Attaches the session to the current physical thread

  @param session  Session handle

  @returns
    false   success
    true    failure
*/
bool Srv_session::attach()
{
  int destroy_stack_start;

  DBUG_ENTER("Srv_session::attach");
  DBUG_PRINT("info",("current_thd=%p", current_thd));

  if (wait_to_attach())
  {
    DBUG_RETURN(true);
  }

  if (&thd_ == current_thd)
  {
    DBUG_PRINT("info",("&thd_ == current_thd Nothing to do."));
    DBUG_RETURN(false);
  }

  THD *old_thd= current_thd;
  DBUG_PRINT("info",("current_thd=%p", current_thd));

  if (old_thd)
    old_thd->restore_globals();

  const char *new_stack= old_thd? old_thd->thread_stack :
                          (const char*)&destroy_stack_start;

  /*
    Attach optimistically, as this will set thread_stack,
    which needed by store_globals()
  */
  thd_set_thread_stack(&thd_, new_stack);

  // This will install our new THD object as current_thd
  if (thd_.store_globals())
  {
    DBUG_PRINT("error", ("Error while storing globals"));

    if (old_thd)
      old_thd->store_globals();

    set_psi(old_thd);

    set_detached();
    DBUG_RETURN(true);
  }

  DBUG_PRINT("info",("current_thd=%p", current_thd));

  thd_clear_errors(&thd_);

  set_psi(&thd_);

  DBUG_RETURN(false);
}


/**
  Detaches the session from the current physical thread.

  @returns
    false success
    true  failure
*/
bool Srv_session::detach()
{
  DBUG_ENTER("Srv_session::detach");

  DBUG_ASSERT(get_state() == SRV_SESSION_TO_BE_DETACHED ||
              get_state() == SRV_SESSION_ATTACHED ||
              get_state() == SRV_SESSION_CLOSED);

  if (!pthread_equal(thd_.real_id, pthread_self()))
  {
    DBUG_PRINT("error", ("Attached to a different thread. Detach in it"));
    DBUG_RETURN(true);
  }

  DBUG_PRINT("info",("Session=%p THD=%p current_thd=%p",
                     this, get_thd(), current_thd));

  DBUG_ASSERT(&thd_ == current_thd);

  // restore fields
  thd_.protocol = &thd_.protocol_text;
  thd_.set_stmt_da(default_stmt_to_restore_);
  thd_.net.vio = default_vio_to_restore_;

  thd_.restore_globals();

  set_psi(NULL);

  set_conn_thd_id(0);

  /*
    Call after restore_globals() as it will check the stack_addr, which is
    nulled by set_detached()
  */
  set_detached();
  DBUG_RETURN(false);
}


/**
  Sets the security context corresponding to the user on the session thd_.

  @returns
    false success
    true  failure
*/
bool Srv_session::switch_to_user(
    const char *username,
    const char *hostname,
    const char *address,
    const char *db)
{
  DBUG_ENTER(__func__);

  auto scontext = thd_.security_context();
  // free existing fields
  scontext->destroy();

  // allocate memory as ptrs are stored in the context and freed in destructor
  auto user_dup= my_strdup(username, MYF(0));
  auto host_dup= my_strdup(hostname, MYF(0));
  const char* ip_dup = (address && strlen(address))?
                        my_strdup(address, MYF(0)):"";

  if (acl_getroot(scontext, user_dup, host_dup,
                  (char*)ip_dup, (char*)db))
  {
    DBUG_RETURN(true);
  }

  DBUG_PRINT("info", ("Switched security context to user %s@%s [%s]",
                      username, hostname, address));

  DBUG_RETURN(false);
}

/**
  Closes the session

  @returns
    false Session successfully closed
    true  No such session exists / Session is attached to a different thread
*/
bool Srv_session::close()
{
  DBUG_ENTER("Srv_session::close");

  DBUG_PRINT("info",("Session=%p THD=%p current_thd=%p",
                     this, get_thd(), current_thd));

  THD *old_thd= current_thd;

  // attach session to thread
  attach();

  DBUG_ASSERT(get_state() < SRV_SESSION_CLOSED);

  switch_state(SRV_SESSION_CLOSED);

  server_session_list.remove(get_session_id());

  /*
    Log to general log must happen before release_resources() as
    current_thd will be different then.
  */
  MYSQL_AUDIT_NOTIFY_CONNECTION_DISCONNECT(&thd_, 0);

  close_mysql_tables(&thd_);

  set_psi(NULL);

  thd_.release_resources();

  // detach
  detach();

  // Install back old THD object as current_thd
  if (old_thd)
    old_thd->store_globals();

  DBUG_RETURN(false);
}

/**
  Changes the state of a session to detached
*/
void Srv_session::set_detached()
{
  switch_state(SRV_SESSION_DETACHED);
  thd_set_thread_stack(&thd_, NULL);
}

int Srv_session::execute_query(char* packet, uint packet_length,
                                 const CHARSET_INFO * client_cs)
{
  DBUG_ENTER("Srv_session::execute_query");

  if (client_cs &&
      thd_.variables.character_set_results != client_cs &&
      thd_init_client_charset(&thd_, client_cs->number))
    DBUG_RETURN(1);

  mysql_audit_release(&thd_);

  DBUG_ASSERT(thd_.m_statement_psi == NULL);
  thd_.m_statement_psi= MYSQL_START_STATEMENT(&thd_.m_statement_state,
                                             stmt_info_new_packet.m_key,
                                             thd_.db,
                                             thd_.db_length,
                                             thd_.charset());

  int ret = dispatch_command(COM_QUERY, &thd_, packet, packet_length, this);

  DBUG_RETURN(ret);
}


static void append_session_id_in_ok(THD* session_thd) {
  session_thd->get_stmt_da()->set_message("%s:%d",
            Srv_session::RpcIdAttr, session_thd->thread_id());

  DBUG_PRINT("info", ("Sending rpc id in OK %s",
                      session_thd->get_stmt_da()->message()));
}

// Called after query executed and before sending out the OK/Err.
// If session state changed:
// - Appends session id in OK is session state changed
// - Puts session in map if not present
// - Releases session by resetting the conn_thd field while under map lock.
void Srv_session::end_statement() {
  DBUG_ENTER(__func__);

  static LEX_CSTRING key = { STRING_WITH_LEN(RpcIdAttr) };

  if (!session_state_changed()) {
    // remove from session map
    server_session_list.remove(get_session_id());
    DBUG_VOID_RETURN;
  }

  if (!thd_.is_error()) {
    append_session_id_in_ok(&thd_);
    auto tracker = session_tracker_->get_tracker(SESSION_RESP_ATTR_TRACKER);
    if (tracker->is_enabled())
    {
      char tmp[21];
      snprintf(tmp, sizeof(tmp), "%llu", (ulonglong) thd_.thread_id());
      LEX_CSTRING value = { tmp, strlen(tmp) };
      tracker->mark_as_changed(current_thd, &key, &value);
    }
  }

  // Mark that session will be detached after finishing sending response out
  // so if next in session query comes on another connection thread it can wait
  // until session is detached.
  switch_state(SRV_SESSION_TO_BE_DETACHED);

  DBUG_VOID_RETURN;
}

/* Valid state changes */
std::unordered_map<Srv_session::srv_session_state,
                  std::vector<Srv_session::srv_session_state>,
                  std::hash<int>> valid_state_changes = {
  {Srv_session::SRV_SESSION_CREATED, {Srv_session::SRV_SESSION_ATTACHED}},
  {Srv_session::SRV_SESSION_ATTACHED,
      {Srv_session::SRV_SESSION_TO_BE_DETACHED,
       Srv_session::SRV_SESSION_DETACHED}},
  {Srv_session::SRV_SESSION_TO_BE_DETACHED,
      {Srv_session::SRV_SESSION_DETACHED}},
  {Srv_session::SRV_SESSION_DETACHED, {Srv_session::SRV_SESSION_ATTACHED}},
};

bool check_state_change(Srv_session::srv_session_state old_state,
                        Srv_session::srv_session_state new_state) {
  if (new_state == Srv_session::SRV_SESSION_CLOSED) {
    // session can be freed at any time, timeout, error
    return true;
  }

  auto vec_ptr = valid_state_changes.find(old_state);
  return (vec_ptr != valid_state_changes.end() &&
          std::find(vec_ptr->second.begin(), vec_ptr->second.end(),
                    new_state) != vec_ptr->second.end());
}


void Srv_session::switch_state_safe(srv_session_state new_state) {
  if (state_ == Srv_session::SRV_SESSION_CLOSED) {
    return;
  }

  if (!check_state_change(state_, new_state)) {
    DBUG_PRINT("error", ("Invalid state switch from %d to %d",
                          state_, new_state));
    DBUG_ASSERT(false);
  }

  auto prev_state = state_;
  state_ = new_state;
  // if was in to be detached, notify thread that might have received the
  // next in session query and waiting for session to be detached.
  if (prev_state == SRV_SESSION_TO_BE_DETACHED) {
    wait_to_attach_.notify_all();
  }
  DBUG_PRINT("info", ("switch session state %p from %d to %d",
                      this, prev_state, new_state));
}

void Srv_session::switch_state(srv_session_state new_state) {
  std::lock_guard<std::mutex> lock(mutex_);
  switch_state_safe(new_state);
}
