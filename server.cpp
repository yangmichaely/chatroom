#include <iostream>
#include <thread>
#include <cstring>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <atomic>
#include <signal.h>

const int PORT = 12345;
const int BACKLOG = 2;
const int BUFFER_SIZE = 4096;

// global flag to control thread termination
std::atomic<bool> shouldTerminate(false);

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
    perror("accept");
    exit(EXIT_FAILURE);
  }
  return client_fd;
}

void relayMessages(int fromClientFd, int toClientFd)
{
  char buffer[BUFFER_SIZE];
  while (!shouldTerminate)
  {
    ssize_t bytesRead = read(fromClientFd, buffer, sizeof(buffer));
    if (bytesRead == 0)
    {
      std::cerr << "client disconnected." << std::endl;
      shouldTerminate = true; // signal other threads to terminate
      break;
    }
    if (bytesRead < 0)
    {
      if (errno == EINTR)
        continue; // interrupted, retry
      perror("read failed");
      shouldTerminate = true; // signal other threads to terminate
      break;
    }

    ssize_t totalWritten = 0;
    while (totalWritten < bytesRead && !shouldTerminate)
    {
      ssize_t bytesWritten = write(toClientFd, buffer + totalWritten, bytesRead - totalWritten);
      if (bytesWritten <= 0)
      {
        if (errno == EINTR)
          continue;
        perror("write failed");
        shouldTerminate = true; // signal other threads to terminate
        return;
      }
      totalWritten += bytesWritten;
    }
  }
}

int main()
{
  // set up signal handlers for graceful shutdown
  signal(SIGINT, signalHandler);   // ctrl c
  signal(SIGTERM, signalHandler);  // termination signal
  signal(SIGPIPE, SIG_IGN);        // ignore when user disconnects
  
  int server_fd = createServerSocket(PORT);

  // assuming only two clients for now
  int client1_fd = acceptClient(server_fd);
  int client2_fd = acceptClient(server_fd);

  std::thread t1(relayMessages, client1_fd, client2_fd);
  std::thread t2(relayMessages, client2_fd, client1_fd);
  t1.join();
  t2.join();

  // cleanup
  close(client1_fd);
  close(client2_fd);
  close(server_fd);
  return 0;
}