#include <iostream>
#include <cstring>
#include <thread>
#include <atomic>
#include <signal.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>

// global flag to control thread termination
std::atomic<bool> running(true);

int createClientSocket(const char *ip, int port)
{
  // similar to server socket creation, but using the connect function
  int client_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (client_fd < 0)
  {
    perror("socket failed");
    exit(EXIT_FAILURE);
  }

  struct sockaddr_in server_addr;
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(port);

  // converting ipv4 to binary, assuming the server is running on my pc
  if (inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr) <= 0)
  {
    perror("invalid address");
    exit(EXIT_FAILURE);
  }

  // connection!
  if (connect(client_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
  {
    perror("connect failed");
    close(client_fd);
    exit(EXIT_FAILURE);
  }

  return client_fd;
}

int sendMessage(int client_fd, const std::string &message)
{
  ssize_t bytes_sent = send(client_fd, message.c_str(), message.size(), 0);
  if (bytes_sent < 0)
  {
    perror("send failed");
    return -1;
  }
  return bytes_sent;
}

int receiveMessage(int client_fd, char *buffer, size_t buffer_size)
{
  ssize_t bytes_received = recv(client_fd, buffer, buffer_size - 1, 0);
  if (bytes_received < 0)
  {
    perror("recv failed");
    return -1;
  }
  if (bytes_received == 0)
  {
    // connection closed by server
    return 0;
  }
  buffer[bytes_received] = '\0';
  return bytes_received;
}

void sendThread(int client_fd)
{
  std::string message;
  while (running)
  {
    std::cout << "enter message (type quit to exit): ";
    std::getline(std::cin, message);
    
    if (message == "quit")
    {
      running = false;
      break;
    }

    if (sendMessage(client_fd, message) < 0)
    {
      std::cerr << "failed to send message" << std::endl;
      running = false;
      break;
    }
  }
}

void receiveThread(int client_fd)
{
  char buffer[1024];
  while (running)
  {
    int result = receiveMessage(client_fd, buffer, sizeof(buffer));
    if (result < 0)
    {
      std::cerr << "failed to receive message" << std::endl;
      running = false;
      break;
    }
    if (result == 0)
    {
      std::cout << "server disconnected" << std::endl;
      running = false;
      break;
    }
    std::cout << "server: " << buffer << std::endl;
  }
}

int main()
{
  const char *server_ip = "127.0.0.1";
  int server_port = 12345;

  int client_fd = createClientSocket(server_ip, server_port);
  if (client_fd < 0)
  {
    return -1;
  }

  std::cout << "connected to server. type quit to exit." << std::endl;

  std::thread send_thread(sendThread, client_fd);
  std::thread receive_thread(receiveThread, client_fd);

  send_thread.join();
  receive_thread.join();

  close(client_fd);
  std::cout << "client disconnected." << std::endl;
  return 0;
}