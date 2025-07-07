#include <iostream>
#include <thread>
#include <cstring>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <atomic>
#include <signal.h>
#include <vector>
#include <mutex>
#include <algorithm>
#include <errno.h>

const int PORT = 12345;
const int BACKLOG = 10;
const int BUFFER_SIZE = 4096;

// global flag to control thread termination
std::atomic<bool> shouldTerminate(false);

// store all connected clients
std::vector<int> clients;
std::mutex clientsMutex;

void signalHandler(int signal)
{
  std::cout << "\nreceived signal " << signal << ". shutting down..." << std::endl;
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
  struct sockaddr_in client_addr;
  socklen_t addr_len = sizeof(client_addr);
  int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
  if (client_fd < 0)
  {
    if (shouldTerminate)
    {
      return -1; // server is shutting down
    }
    perror("accept");
    return -1;
  }
  return client_fd;
}

void addClient(int clientFd)
{
  std::lock_guard<std::mutex> lock(clientsMutex);
  clients.push_back(clientFd);
  std::cout << "client connected. total clients: " << clients.size() << std::endl;
}

void removeClient(int clientFd)
{
  std::lock_guard<std::mutex> lock(clientsMutex);
  clients.erase(std::remove(clients.begin(), clients.end(), clientFd), clients.end());
  std::cout << "client disconnected. total clients: " << clients.size() << std::endl;
}

void broadcastMessage(int senderFd, const char* buffer, ssize_t bytesRead)
{
  std::lock_guard<std::mutex> lock(clientsMutex);
  for (int clientFd : clients)
  {
    if (clientFd != senderFd) // don't send message back to sender
    {
      ssize_t totalWritten = 0;
      while (totalWritten < bytesRead && !shouldTerminate)
      {
        ssize_t bytesWritten = write(clientFd, buffer + totalWritten, bytesRead - totalWritten);
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
  addClient(clientFd);
  
  while (!shouldTerminate)
  {
    ssize_t bytesRead = read(clientFd, buffer, sizeof(buffer));
    if (bytesRead == 0)
    {
      std::cerr << "client disconnected." << std::endl;
      break;
    }
    if (bytesRead < 0)
    {
      if (errno == EINTR)
        continue; // interrupted, retry
      perror("read failed");
      break;
    }

    // broadcast message to all other clients
    broadcastMessage(clientFd, buffer, bytesRead);
  }
  
  removeClient(clientFd);
  close(clientFd);
}

int main()
{
  // set up signal handlers for graceful shutdown
  signal(SIGINT, signalHandler);   // ctrl c
  signal(SIGTERM, signalHandler);  // termination signal
  signal(SIGPIPE, SIG_IGN);        // ignore when user disconnects
  
  int server_fd = createServerSocket(PORT);

  std::vector<std::thread> clientThreads;

  // continuously accept new clients
  while (!shouldTerminate)
  {
    int client_fd = acceptClient(server_fd);
    if (client_fd >= 0)
    {
      // create a new thread to handle this client
      clientThreads.emplace_back(handleClient, client_fd);
    }
  }

  // wait for all client threads to finish
  for (auto& thread : clientThreads)
  {
    if (thread.joinable())
    {
      thread.join();
    }
  }

  // cleanup
  {
    std::lock_guard<std::mutex> lock(clientsMutex);
    for (int clientFd : clients)
    {
      close(clientFd);
    }
  }
  close(server_fd);
  return 0;
}