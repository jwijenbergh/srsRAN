/**
 * Copyright 2013-2021 Software Radio Systems Limited
 *
 * This file is part of srsLTE.
 *
 * srsLTE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsLTE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#include "srsran/common/network_utils.h"

#include <netinet/sctp.h>
#include <sys/socket.h>
#include <sys/types.h>

#define rxSockError(fmt, ...) logger.error("%s: " fmt, name.c_str(), ##__VA_ARGS__)
#define rxSockWarn(fmt, ...) logger.warning("%s: " fmt, name.c_str(), ##__VA_ARGS__)
#define rxSockInfo(fmt, ...) logger.info("%s: " fmt, name.c_str(), ##__VA_ARGS__)
#define rxSockDebug(fmt, ...) logger.debug("%s: " fmt, name.c_str(), ##__VA_ARGS__)

namespace srsran {

const char* LOGSERVICE = "COMN";

namespace net_utils {

//! Set IP:port for ipv4
bool set_sockaddr(sockaddr_in* addr, const char* ip_str, int port)
{
  addr->sin_family = AF_INET;
  if (inet_pton(addr->sin_family, ip_str, &addr->sin_addr) != 1) {
    perror("inet_pton");
    return false;
  }
  addr->sin_port = htons(port);
  return true;
}

//! Set IP:port for ipv6
bool set_sockaddr(sockaddr_in6* addr, const char* ip_str, int port)
{
  addr->sin6_family = AF_INET6;
  if (inet_pton(addr->sin6_family, ip_str, &addr->sin6_addr) != 1) {
    perror("inet_pton for ipv6");
    return false;
  }
  addr->sin6_port = htons(port);
  return true;
}

std::string get_ip(const sockaddr_in& addr)
{
  char ip_str[INET_ADDRSTRLEN];
  if (inet_ntop(AF_INET, &addr.sin_addr, ip_str, sizeof(ip_str)) == nullptr) {
    return "<bad ipv4 address>";
  }
  return std::string{ip_str};
}

std::string get_ip(const sockaddr_in6& addr)
{
  char ip_str[INET6_ADDRSTRLEN];
  if (inet_ntop(AF_INET6, &addr.sin6_addr, ip_str, sizeof(ip_str)) == nullptr) {
    return "<bad ipv6 address>";
  }
  return std::string{ip_str};
}

int get_port(const sockaddr_in& addr)
{
  return ntohs(addr.sin_port);
}

net_utils::socket_type get_addr_family(int fd)
{
  if (fd < 0) {
    return net_utils::socket_type::none;
  }
  int       type;
  socklen_t length = sizeof(int);
  getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &length);
  return (net_utils::socket_type)type;
}

const char* protocol_to_string(protocol_type p)
{
  switch (p) {
    case protocol_type::TCP:
      return "TCP";
    case protocol_type::UDP:
      return "UDP";
    case protocol_type::SCTP:
      return "SCTP";
    default:
      break;
  }
  return "";
}

int open_socket(net_utils::addr_family ip_type, net_utils::socket_type socket_type, net_utils::protocol_type protocol)
{
  int fd = socket((int)ip_type, (int)socket_type, (int)protocol);
  if (fd == -1) {
    srslog::fetch_basic_logger(LOGSERVICE).error("Failed to open %s socket.", net_utils::protocol_to_string(protocol));
    perror("Could not create socket\n");
    return -1;
  }

  if (protocol == protocol_type::SCTP) {
    // Sets the data_io_event to be able to use sendrecv_info
    // Subscribes to the SCTP_SHUTDOWN event, to handle graceful shutdown
    // Also subscribes to SCTP_PEER_ADDR_CHANGE, to handle ungraceful shutdown of the link.
    struct sctp_event_subscribe evnts = {};
    evnts.sctp_data_io_event          = 1;
    evnts.sctp_shutdown_event         = 1;
    evnts.sctp_address_event          = 1;
    if (setsockopt(fd, IPPROTO_SCTP, SCTP_EVENTS, &evnts, sizeof(evnts)) != 0) {
      srslog::fetch_basic_logger(LOGSERVICE).error("Failed to subscribe to SCTP_SHUTDOWN event: %s", strerror(errno));
      perror("Could not regiester socket to SCTP events\n");
    }

    /*
     * Modify SCTP default parameters for quicker detection of broken links.
     * This includes changes to the SCTP_INITMSG parameters (to control the timeout of the connect() syscall)
     * And changes to the maximum re-transmission timeout (rto_max), for quicker detection of broken links.
     */
    // Set RTO_MAX to quickly detect broken links.
    sctp_rtoinfo rto_opts;
    socklen_t    rto_sz    = sizeof(sctp_rtoinfo);
    rto_opts.srto_assoc_id = 0;
    if (getsockopt(fd, SOL_SCTP, SCTP_RTOINFO, &rto_opts, &rto_sz) < 0) {
      printf("Error getting RTO_INFO sockopts\n");
      return -1;
    }

    rto_opts.srto_max = 6000; // 6 seconds

    srslog::fetch_basic_logger(LOGSERVICE)
        .debug(
            "Setting RTO_INFO options on SCTP socket. Association %d, Initial RTO %d, Minimum RTO %d, Maximum RTO %d",
            rto_opts.srto_assoc_id,
            rto_opts.srto_initial,
            rto_opts.srto_min,
            rto_opts.srto_max);

    if (setsockopt(fd, SOL_SCTP, SCTP_RTOINFO, &rto_opts, rto_sz) < 0) {
      perror("Error setting RTO_INFO sockopts\n");
      return -1;
    }

    // Set SCTP INITMSG options to reduce blocking timeout of connect()
    sctp_initmsg init_opts;
    socklen_t    init_sz = sizeof(sctp_initmsg);
    if (getsockopt(fd, SOL_SCTP, SCTP_INITMSG, &init_opts, &init_sz) < 0) {
      printf("Error getting sockopts\n");
    }

    init_opts.sinit_max_attempts   = 3;
    init_opts.sinit_max_init_timeo = 5000; // 5 seconds

    srslog::fetch_basic_logger(LOGSERVICE)
        .debug("Setting SCTP_INITMSG options on SCTP socket. Max attempts %d, Max init attempts timeout %d",
               init_opts.sinit_max_attempts,
               init_opts.sinit_max_init_timeo);
    if (setsockopt(fd, SOL_SCTP, SCTP_INITMSG, &init_opts, init_sz) < 0) {
      perror("Error setting SCTP_INITMSG sockopts\n");
      return -1;
    }
  }

  return fd;
}

bool bind_addr(int fd, const sockaddr_in& addr_in)
{
  if (fd < 0) {
    srslog::fetch_basic_logger(LOGSERVICE).error("Trying to bind to a closed socket");
    return false;
  }

  if (bind(fd, (struct sockaddr*)&addr_in, sizeof(addr_in)) != 0) {
    srslog::fetch_basic_logger(LOGSERVICE)
        .error("Failed to bind on address %s: %s errno %d", get_ip(addr_in).c_str(), strerror(errno), errno);
    perror("bind()");
    return false;
  }
  return true;
}

bool bind_addr(int fd, const char* bind_addr_str, int port, sockaddr_in* addr_result)
{
  sockaddr_in addr_tmp{};
  if (not net_utils::set_sockaddr(&addr_tmp, bind_addr_str, port)) {
    srslog::fetch_basic_logger(LOGSERVICE)
        .error("Failed to convert IP address (%s) to sockaddr_in struct", bind_addr_str);
    return false;
  }
  bind_addr(fd, addr_tmp);
  if (addr_result != nullptr) {
    *addr_result = addr_tmp;
  }
  return true;
}

bool connect_to(int fd, const char* dest_addr_str, int dest_port, sockaddr_in* dest_sockaddr)
{
  if (fd < 0) {
    srslog::fetch_basic_logger(LOGSERVICE).error("tried to connect to remote address with an invalid socket.");
    return false;
  }
  sockaddr_in sockaddr_tmp{};
  if (not net_utils::set_sockaddr(&sockaddr_tmp, dest_addr_str, dest_port)) {
    srslog::fetch_basic_logger(LOGSERVICE)
        .error("Error converting IP address (%s) to sockaddr_in structure", dest_addr_str);
    return false;
  }
  if (dest_sockaddr != nullptr) {
    *dest_sockaddr = sockaddr_tmp;
  }
  if (connect(fd, (const struct sockaddr*)&sockaddr_tmp, sizeof(sockaddr_tmp)) == -1) {
    srslog::fetch_basic_logger(LOGSERVICE).info("Failed to establish socket connection to %s", dest_addr_str);
    perror("connect()");
    return false;
  }
  return true;
}

} // namespace net_utils

/********************************************
 *           Socket Classes
 *******************************************/

socket_handler_t::socket_handler_t(socket_handler_t&& other) noexcept
{
  sockfd       = other.sockfd;
  addr         = other.addr;
  other.sockfd = 0;
  other.addr   = {};
}
socket_handler_t::~socket_handler_t()
{
  reset();
}
socket_handler_t& socket_handler_t::operator=(socket_handler_t&& other) noexcept
{
  if (this == &other) {
    return *this;
  }
  addr         = other.addr;
  sockfd       = other.sockfd;
  other.addr   = {};
  other.sockfd = 0;
  return *this;
}

void socket_handler_t::close()
{
  if (sockfd >= 0) {
    ::close(sockfd);
    sockfd = -1;
  }
}

void socket_handler_t::reset()
{
  this->close();
  addr = {};
}

bool socket_handler_t::bind_addr(const char* bind_addr_str, int port)
{
  return net_utils::bind_addr(sockfd, bind_addr_str, port, &addr);
}

bool socket_handler_t::connect_to(const char* dest_addr_str, int dest_port, sockaddr_in* dest_sockaddr)
{
  return net_utils::connect_to(sockfd, dest_addr_str, dest_port, dest_sockaddr);
}

bool socket_handler_t::open_socket(net_utils::addr_family   ip_type,
                                   net_utils::socket_type   socket_type,
                                   net_utils::protocol_type protocol)
{
  if (sockfd >= 0) {
    srslog::fetch_basic_logger(LOGSERVICE).error("Socket is already open.");
    return false;
  }
  sockfd = net_utils::open_socket(ip_type, socket_type, protocol);
  return sockfd >= 0;
}

/***********************************************************************
 *                          SCTP socket
 **********************************************************************/

namespace net_utils {

bool sctp_init_socket(socket_handler_t* socket, net_utils::socket_type socktype, const char* bind_addr_str, int port)
{
  if (not socket->open_socket(net_utils::addr_family::ipv4, socktype, net_utils::protocol_type::SCTP)) {
    return false;
  }
  if (not socket->bind_addr(bind_addr_str, port)) {
    socket->reset();
    return false;
  }
  return true;
}

bool sctp_init_client(socket_handler_t* socket, net_utils::socket_type socktype, const char* bind_addr_str)
{
  return sctp_init_socket(socket, socktype, bind_addr_str, 0);
}

bool sctp_init_server(socket_handler_t* socket, net_utils::socket_type socktype, const char* bind_addr_str, int port)
{
  if (not sctp_init_socket(socket, socktype, bind_addr_str, port)) {
    return false;
  }
  // Listen for connections
  if (listen(socket->fd(), SOMAXCONN) != 0) {
    srslog::fetch_basic_logger(LOGSERVICE).error("Failed to listen to incoming SCTP connections");
    return false;
  }
  return true;
}

/***************************************************************
 *                        TCP Socket
 **************************************************************/

bool tcp_make_server(socket_handler_t* socket, const char* bind_addr_str, int port, int nof_connections)
{
  if (not socket->open_socket(addr_family::ipv4, socket_type::stream, protocol_type::TCP)) {
    return false;
  }
  if (not socket->bind_addr(bind_addr_str, port)) {
    socket->reset();
    return false;
  }
  // Listen for connections
  if (listen(socket->fd(), nof_connections) != 0) {
    srslog::fetch_basic_logger(LOGSERVICE).error("Failed to listen to incoming TCP connections");
    return false;
  }
  return true;
}

int tcp_accept(socket_handler_t* socket, sockaddr_in* destaddr)
{
  socklen_t clilen = sizeof(destaddr);
  int       connfd = accept(socket->fd(), (struct sockaddr*)&destaddr, &clilen);
  if (connfd < 0) {
    srslog::fetch_basic_logger(LOGSERVICE).error("Failed to accept connection");
    perror("accept");
    return -1;
  }
  return connfd;
}

int tcp_read(int remotefd, void* buf, size_t nbytes)
{
  int n = ::read(remotefd, buf, nbytes);
  if (n == 0) {
    srslog::fetch_basic_logger(LOGSERVICE).info("TCP connection closed");
    close(remotefd);
    return 0;
  }
  if (n == -1) {
    srslog::fetch_basic_logger(LOGSERVICE).error("Failed to read from TCP socket.");
    perror("TCP read");
  }
  return n;
}

int tcp_send(int remotefd, const void* buf, size_t nbytes)
{
  // Loop until all bytes are sent
  char*   ptr              = (char*)buf;
  ssize_t nbytes_remaining = nbytes;
  while (nbytes_remaining > 0) {
    ssize_t i = ::send(remotefd, ptr, nbytes_remaining, 0);
    if (i < 1) {
      srslog::fetch_basic_logger(LOGSERVICE).error("Failed to send data to TCP socket");
      perror("Error calling send()\n");
      return i;
    }
    ptr += i;
    nbytes_remaining -= i;
  }
  return nbytes - nbytes_remaining;
}

} // namespace net_utils

/***************************************************************
 *                 Rx Multisocket Task Types
 **************************************************************/

/**
 * Description: Specialization of recv_task for the case the received data is
 * in the form of unique_byte_buffer, and a recvfrom(...) call is used
 */
class recvfrom_pdu_task final : public rx_multisocket_handler::recv_task
{
public:
  using callback_t = std::function<void(srsran::unique_byte_buffer_t pdu, const sockaddr_in& from)>;
  explicit recvfrom_pdu_task(srslog::basic_logger& logger, callback_t func_) : logger(logger), func(std::move(func_)) {}

  bool operator()(int fd) override
  {
    srsran::unique_byte_buffer_t pdu = srsran::make_byte_buffer();
    if (pdu == nullptr) {
      logger.error("Unable to allocate byte buffer");
      return true;
    }
    sockaddr_in from    = {};
    socklen_t   fromlen = sizeof(from);

    ssize_t n_recv = recvfrom(fd, pdu->msg, pdu->get_tailroom(), 0, (struct sockaddr*)&from, &fromlen);
    if (n_recv == -1 and errno != EAGAIN) {
      logger.error("Error reading from socket: %s", strerror(errno));
      return true;
    }
    if (n_recv == -1 and errno == EAGAIN) {
      logger.debug("Socket timeout reached");
      return true;
    }

    pdu->N_bytes = static_cast<uint32_t>(n_recv);
    func(std::move(pdu), from);
    return true;
  }

private:
  srslog::basic_logger& logger;
  callback_t            func;
};

class sctp_recvmsg_pdu_task final : public rx_multisocket_handler::recv_task
{
public:
  using callback_t = std::function<
      void(srsran::unique_byte_buffer_t pdu, const sockaddr_in& from, const sctp_sndrcvinfo& sri, int flags)>;
  explicit sctp_recvmsg_pdu_task(srslog::basic_logger& logger, callback_t func_) :
    logger(logger), func(std::move(func_))
  {}

  bool operator()(int fd) override
  {
    // inside rx_sockets thread. Read socket
    srsran::unique_byte_buffer_t pdu = srsran::make_byte_buffer();
    if (pdu == nullptr) {
      logger.error("Unable to allocate byte buffer");
      return true;
    }
    sockaddr_in     from    = {};
    socklen_t       fromlen = sizeof(from);
    sctp_sndrcvinfo sri     = {};
    int             flags   = 0;
    ssize_t n_recv = sctp_recvmsg(fd, pdu->msg, pdu->get_tailroom(), (struct sockaddr*)&from, &fromlen, &sri, &flags);
    if (n_recv == -1 and errno != EAGAIN) {
      logger.error("Error reading from SCTP socket: %s", strerror(errno));
      return true;
    }
    if (n_recv == -1 and errno == EAGAIN) {
      logger.debug("Socket timeout reached");
      return true;
    }

    bool ret     = true;
    pdu->N_bytes = static_cast<uint32_t>(n_recv);
    // SCTP notifications handled in callback.
    func(std::move(pdu), from, sri, flags);
    return ret;
  }

private:
  srslog::basic_logger& logger;
  callback_t            func;
};

/***************************************************************
 *                 Rx Multisocket Handler
 **************************************************************/

rx_multisocket_handler::rx_multisocket_handler(std::string name_, srslog::basic_logger& logger, int thread_prio) :
  thread(name_), name(std::move(name_)), logger(logger)
{
  // register control pipe fd
  if (pipe(pipefd) == -1) {
    rxSockInfo("Failed to open control pipe");
    return;
  }
  start(thread_prio);
}

rx_multisocket_handler::~rx_multisocket_handler()
{
  stop();
}

void rx_multisocket_handler::stop()
{
  if (running) {
    // close thread
    {
      std::lock_guard<std::mutex> lock(socket_mutex);
      ctrl_cmd_t                  msg{};
      msg.cmd = ctrl_cmd_t::cmd_id_t::EXIT;
      if (write(pipefd[1], &msg, sizeof(msg)) != sizeof(msg)) {
        rxSockError("while writing to control pipe");
      }
    }
    rxSockDebug("Closing rx socket handler thread");
    wait_thread_finish();
  }

  if (pipefd[0] >= 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    pipefd[0] = -1;
    pipefd[1] = -1;
    rxSockDebug("closed.");
  }
}

/**
 * Convenience method for read PDUs from socket
 */
bool rx_multisocket_handler::add_socket_pdu_handler(int fd, recvfrom_callback_t pdu_task)
{
  std::unique_ptr<srsran::rx_multisocket_handler::recv_task> task;
  task.reset(new srsran::recvfrom_pdu_task(logger, std::move(pdu_task)));
  return add_socket_handler(fd, std::move(task));
}

/**
 * Convenience method for reading PDUs from SCTP socket
 */
bool rx_multisocket_handler::add_socket_sctp_pdu_handler(int fd, sctp_recv_callback_t pdu_task)
{
  srsran::rx_multisocket_handler::task_callback_t task;
  task.reset(new srsran::sctp_recvmsg_pdu_task(logger, std::move(pdu_task)));
  return add_socket_handler(fd, std::move(task));
}

bool rx_multisocket_handler::add_socket_handler(int fd, task_callback_t handler)
{
  std::lock_guard<std::mutex> lock(socket_mutex);
  if (fd < 0) {
    rxSockError("Provided SCTP socket must be already open");
    return false;
  }
  if (active_sockets.count(fd) > 0) {
    rxSockError("Tried to register fd=%d, but this fd already exists", fd);
    return false;
  }

  active_sockets.insert(std::pair<const int, task_callback_t>(fd, std::move(handler)));

  // this unlocks the reading thread to add new connections
  ctrl_cmd_t msg;
  msg.cmd    = ctrl_cmd_t::cmd_id_t::NEW_FD;
  msg.new_fd = fd;
  if (write(pipefd[1], &msg, sizeof(msg)) != sizeof(msg)) {
    rxSockError("while writing to control pipe");
    return false;
  }

  rxSockDebug("socket fd=%d has been registered.", fd);
  return true;
}

bool rx_multisocket_handler::remove_socket(int fd)
{
  std::lock_guard<std::mutex> lock(socket_mutex);
  auto                        it = active_sockets.find(fd);
  if (it == active_sockets.end()) {
    rxSockError("The socket fd=%d to be removed does not exist", fd);
    return false;
  }

  ctrl_cmd_t msg;
  msg.cmd    = ctrl_cmd_t::cmd_id_t::RM_FD;
  msg.new_fd = fd;
  if (write(pipefd[1], &msg, sizeof(msg)) != sizeof(msg)) {
    rxSockError("while writing to control pipe");
    return false;
  }
  return true;
}

std::map<int, rx_multisocket_handler::task_callback_t>::iterator
rx_multisocket_handler::remove_socket_unprotected(int fd, fd_set* total_fd_set, int* max_fd)
{
  if (fd < 0) {
    rxSockError("fd to be removed is not valid");
    return active_sockets.end();
  }
  auto it = active_sockets.find(fd);
  it      = active_sockets.erase(it);
  FD_CLR(fd, total_fd_set);
  // assumes ordering
  *max_fd = (active_sockets.empty()) ? pipefd[0] : std::max(pipefd[0], active_sockets.rbegin()->first);
  rxSockDebug("Socket fd=%d has been successfully removed", fd);
  return it;
}

void rx_multisocket_handler::run_thread()
{
  running = true;
  fd_set total_fd_set, read_fd_set;
  FD_ZERO(&total_fd_set);
  int max_fd = 0;

  FD_SET(pipefd[0], &total_fd_set);
  max_fd = std::max(pipefd[0], max_fd);

  while (running) {
    memcpy(&read_fd_set, &total_fd_set, sizeof(total_fd_set));
    int n = select(max_fd + 1, &read_fd_set, nullptr, nullptr, nullptr);

    // handle select return
    if (n == -1) {
      rxSockError("Error from select(%d,...). Number of rx sockets: %d", max_fd + 1, (int)active_sockets.size() + 1);
      continue;
    }
    if (n == 0) {
      rxSockDebug("No data from select.");
      continue;
    }

    // Shared state area
    std::lock_guard<std::mutex> lock(socket_mutex);

    // call read callback for all SCTP/TCP/UDP connections
    for (auto handler_it = active_sockets.begin(); handler_it != active_sockets.end();) {
      int        fd       = handler_it->first;
      recv_task* callback = handler_it->second.get();
      if (not FD_ISSET(fd, &read_fd_set)) {
        ++handler_it;
        continue;
      }
      bool socket_valid = callback->operator()(fd);
      if (not socket_valid) {
        rxSockInfo("The socket fd=%d has been closed by peer", fd);
        handler_it = remove_socket_unprotected(fd, &total_fd_set, &max_fd);
      } else {
        ++handler_it;
      }
    }

    // handle ctrl messages
    if (FD_ISSET(pipefd[0], &read_fd_set)) {
      ctrl_cmd_t msg;
      ssize_t    nrd = read(pipefd[0], &msg, sizeof(msg));
      if (nrd <= 0) {
        rxSockError("Unable to read control message.");
        continue;
      }
      switch (msg.cmd) {
        case ctrl_cmd_t::cmd_id_t::EXIT:
          running = false;
          return;
        case ctrl_cmd_t::cmd_id_t::NEW_FD:
          if (msg.new_fd >= 0) {
            FD_SET(msg.new_fd, &total_fd_set);
            max_fd = std::max(max_fd, msg.new_fd);
          } else {
            rxSockError("added fd is not valid");
          }
          break;
        case ctrl_cmd_t::cmd_id_t::RM_FD:
          remove_socket_unprotected(msg.new_fd, &total_fd_set, &max_fd);
          rxSockDebug("Socket fd=%d has been successfully removed", msg.new_fd);
          break;
        default:
          rxSockError("ctrl message command %d is not valid", (int)msg.cmd);
      }
    }
  }
}

} // namespace srsran
