#include <iostream>
#include <thread>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <atomic>
#include <signal.h>
#include <vector>
#include <mutex>
#include <algorithm>
#include <errno.h>
#include <map>
#include <string>
#include <fcntl.h>
#include <sys/select.h>

const int DEFAULT_PORT = 12345;
const int BACKLOG = 10;
const int BUFFER_SIZE = 4096;

// global flag to control thread termination
std::atomic<bool> shouldTerminate(false);

// store all connected clients with their usernames
std::map<int, std::string> clientUsernames;
std::mutex clientsMutex;

void signalHandler(int signal)
{
  std::cout << ". shutting down..." << std::endl;
  shouldTerminate = true;
}

int createServerSocket(int port)
{
  // making a socket for ipv4 addresses and tcp connections
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd == -1)
  {
    perror("socket failed");
    exit(EXIT_FAILURE);
  }

  // allowing the socket to be rebinded
  int opt = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in address;
  address.sin_family = AF_INET;         // ipv4
  address.sin_addr.s_addr = INADDR_ANY; // binding to all interfaces
  address.sin_port = htons(port);       // converting port number to network byte order

  // binding
  if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0)
  {
    perror("bind failed");
    close(server_fd);
    exit(EXIT_FAILURE);
  }

  // listening
  if (listen(server_fd, BACKLOG) < 0)
  {
    perror("listen failed");
    close(server_fd);
    exit(EXIT_FAILURE);
  }
  std::cout << "server is listening on port " << port << std::endl;
  return server_fd;
}

int acceptClient(int server_fd)
{
  // use select to check if there's a pending connection with timeout
  fd_set readfds;
  struct timeval timeout;

  FD_ZERO(&readfds);
  FD_SET(server_fd, &readfds);

  timeout.tv_sec = 1; // 1 second timeout
  timeout.tv_usec = 0;

  int activity = select(server_fd + 1, &readfds, NULL, NULL, &timeout);

  if (activity < 0)
  {
    if (errno == EINTR)
    {
      return -1; // interrupted by signal
    }
    perror("select");
    return -1;
  }

  if (activity == 0)
  {
    return -1; // timeout, no connection pending
  }

  if (!FD_ISSET(server_fd, &readfds))
  {
    return -1; // no connection pending
  }

  struct sockaddr_in client_addr;
  socklen_t addr_len = sizeof(client_addr);
  int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
  if (client_fd < 0)
  {
    if (shouldTerminate || errno == EINTR)
    {
      return -1; // server is shutting down or interrupted
    }
    perror("accept");
    return -1;
  }
  return client_fd;
}

void addClient(int clientFd, const std::string &username)
{
  std::lock_guard<std::mutex> lock(clientsMutex);
  clientUsernames[clientFd] = username;
  std::cout << "client '" << username << "' connected. total clients: " << clientUsernames.size() << std::endl;
}

void removeClient(int clientFd)
{
  std::lock_guard<std::mutex> lock(clientsMutex);
  auto it = clientUsernames.find(clientFd);
  if (it != clientUsernames.end())
  {
    std::cout << "client '" << it->second << "' disconnected. total clients: " << clientUsernames.size() - 1 << std::endl;
    clientUsernames.erase(it);
  }
}

void broadcastMessage(int senderFd, const std::string &message)
{
  std::lock_guard<std::mutex> lock(clientsMutex);

  std::string senderUsername = "Unknown";
  auto senderIt = clientUsernames.find(senderFd);
  if (senderIt != clientUsernames.end())
  {
    senderUsername = senderIt->second;
  }

  std::string formattedMessage = senderUsername + ": " + message;

  for (const auto &client : clientUsernames)
  {
    if (client.first != senderFd) // don't send message back to sender
    {
      ssize_t totalWritten = 0;
      ssize_t messageLength = formattedMessage.length();
      while (totalWritten < messageLength && !shouldTerminate)
      {
        ssize_t bytesWritten = write(client.first, formattedMessage.c_str() + totalWritten, messageLength - totalWritten);
        if (bytesWritten <= 0)
        {
          if (errno == EINTR)
            continue;
          perror("write failed");
          break;
        }
        totalWritten += bytesWritten;
      }
    }
  }
}

void handleClient(int clientFd)
{
  char buffer[BUFFER_SIZE];

  // first, ask for username
  const char *usernamePrompt = "username: ";
  write(clientFd, usernamePrompt, strlen(usernamePrompt));

  // read username
  ssize_t bytesRead = read(clientFd, buffer, sizeof(buffer) - 1);
  if (bytesRead <= 0)
  {
    close(clientFd);
    return;
  }

  buffer[bytesRead] = '\0';
  // remove newline if present
  if (buffer[bytesRead - 1] == '\n')
  {
    buffer[bytesRead - 1] = '\0';
  }

  std::string username(buffer);

  // check if username is already taken
  {
    std::lock_guard<std::mutex> lock(clientsMutex);
    for (const auto &client : clientUsernames)
    {
      if (client.second == username)
      {
        const char *errorMsg = "Username already taken. Please reconnect with a different username.\n";
        write(clientFd, errorMsg, strlen(errorMsg));
        close(clientFd);
        return;
      }
    }
  }

  addClient(clientFd, username);

  std::string welcomeMsg = "welcome, " + username + "\n";
  write(clientFd, welcomeMsg.c_str(), welcomeMsg.length());

  // notify other clients
  std::string joinMsg = username + " has joined the chatroom";
  broadcastMessage(clientFd, joinMsg);

  while (!shouldTerminate)
  {
    bytesRead = read(clientFd, buffer, sizeof(buffer) - 1);
    if (bytesRead == 0)
    {
      std::cerr << "client '" << username << "' disconnected." << std::endl;
      break;
    }
    if (bytesRead < 0)
    {
      if (errno == EINTR)
        continue; // interrupted, retry
      perror("read failed");
      break;
    }

    buffer[bytesRead] = '\0';
    // remove newline if present
    if (buffer[bytesRead - 1] == '\n')
    {
      buffer[bytesRead - 1] = '\0';
    }

    std::string message(buffer);

    // broadcast message to all other clients
    broadcastMessage(clientFd, message);
  }

  // notify other clients about departure
  std::string leaveMsg = "\n" + username + " has left the chatroom";
  broadcastMessage(clientFd, leaveMsg);

  removeClient(clientFd);
  close(clientFd);
}

int main(int argc, char *argv[])
{
  // default port
  int port = DEFAULT_PORT;

  // parse command line arguments
  if (argc >= 2)
  {
    port = std::atoi(argv[1]);
    if (port <= 0 || port > 65535)
    {
      std::cerr << "Error: Invalid port number. Port must be between 1 and 65535." << std::endl;
      return -1;
    }
  }
  if (argc > 2)
  {
    std::cout << "Usage: " << argv[0] << " [port]" << std::endl;
    std::cout << "  port: Port number (default: " << DEFAULT_PORT << ")" << std::endl;
    return -1;
  }

  signal(SIGINT, signalHandler);  // ctrl c
  signal(SIGTERM, signalHandler); // termination signal
  signal(SIGPIPE, SIG_IGN);       // ignore when user disconnects

  int server_fd = createServerSocket(port);

  std::vector<std::thread> clientThreads;

  std::cout << "server started." << std::endl;

  // continuously accept new clients
  while (!shouldTerminate)
  {
    int client_fd = acceptClient(server_fd);
    if (client_fd >= 0)
    {
      // create a new thread to handle this client
      clientThreads.emplace_back(handleClient, client_fd);
    }
    // small sleep to prevent busy waiting when no connections
    if (!shouldTerminate)
    {
      usleep(10000);
    }
  }

  std::cout << "shutting down server..." << std::endl;

  // wait for all client threads to finish (with timeout)
  for (auto &thread : clientThreads)
  {
    if (thread.joinable())
    {
      thread.join();
    }
  }

  // cleanup
  {
    std::lock_guard<std::mutex> lock(clientsMutex);
    for (const auto &client : clientUsernames)
    {
      close(client.first);
    }
  }
  close(server_fd);

  std::cout << "server shutdown complete." << std::endl;
  return 0;
}