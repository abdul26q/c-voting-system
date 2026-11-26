#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define BUFFER_SIZE 8192

// In-memory database for candidates
typedef struct {
    char name[50];
    int votes;
} Candidate;

Candidate candidates[3] = {
    {"Alice Johnson (Party A)", 0},
    {"Bob Smith (Party B)", 0},
    {"Charlie Davis (Party C)", 0}
};

// Function to send HTTP responses
void send_response(int client_socket, int status_code, const char* status_text, const char* body) {
    char header[1024];
    int body_len = strlen(body);
    
    sprintf(header, 
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/html; charset=UTF-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n", 
        status_code, status_text, body_len);
        
    send(client_socket, header, strlen(header), 0);
    send(client_socket, body, body_len, 0);
}

// Function to generate the HTML Homepage
void generate_homepage(char* buffer) {
    char candidates_html[2048] = "";
    char temp[256];
    
    for (int i = 0; i < 3; i++) {
        sprintf(temp, 
            "<div class='candidate'>"
            "<label><input type='radio' name='candidate' value='%d' required> <strong>%s</strong></label>"
            "<span class='votes'>Votes: %d</span>"
            "</div>", 
            i, candidates[i].name, candidates[i].votes);
        strcat(candidates_html, temp);
    }

    sprintf(buffer,
        "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>C Voting System</title>"
        "<style>"
        "body { font-family: 'Segoe UI', Arial, sans-serif; max-width: 600px; margin: 40px auto; background: #f4f7f6; padding: 30px; border-radius: 10px; box-shadow: 0 4px 15px rgba(0,0,0,0.1); }"
        "h1 { color: #2c3e50; text-align: center; margin-bottom: 10px; }"
        ".subtitle { text-align: center; color: #7f8c8d; margin-bottom: 30px; font-size: 14px;}"
        ".candidate { background: #fff; padding: 15px 20px; margin: 12px 0; border-radius: 8px; display: flex; justify-content: space-between; align-items: center; border: 1px solid #e0e0e0; transition: 0.3s; }"
        ".candidate:hover { border-color: #3498db; box-shadow: 0 2px 8px rgba(52,152,219,0.2); }"
        "button { background: #3498db; color: white; border: none; padding: 12px 25px; border-radius: 5px; cursor: pointer; font-size: 16px; width: 100%; margin-top: 20px; font-weight: bold; transition: 0.3s; }"
        "button:hover { background: #2980b9; }"
        ".votes { font-weight: bold; color: #27ae60; background: #e8f8f5; padding: 5px 10px; border-radius: 4px; }"
        ".footer { text-align: center; margin-top: 30px; font-size: 12px; color: #95a5a6; }"
        "</style></head><body>"
        "<h1>🗳️ University Election 2026</h1>"
        "<p class='subtitle'>Powered by a Custom C HTTP Server</p>"
        "<form method='POST' action='/vote'>"
        "%s"
        "<button type='submit'>Cast My Vote</button>"
        "</form>"
        "<div class='footer'>Built with C Sockets | No frameworks used</div>"
        "</body></html>",
        candidates_html);
}

// Function to generate the Success Page
void generate_success_page(char* buffer) {
    sprintf(buffer,
        "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>Vote Cast</title>"
        "<style>body{font-family:Arial;text-align:center;margin-top:100px;background:#f4f7f6;}"
        ".box{background:#fff;padding:40px;border-radius:10px;box-shadow:0 4px 15px rgba(0,0,0,0.1);display:inline-block;}"
        "a{color:#3498db;text-decoration:none;font-weight:bold;}</style></head>"
        "<body><div class='box'><h1 style='color:#27ae60;'>✅ Vote Recorded Successfully!</h1>"
        "<p>Thank you for participating in the election.</p>"
        "<p><a href='/'>← Back to Live Results</a></p></div></body></html>");
}

int main() {
    int server_fd, new_socket;
    struct sockaddr_in address;
    int addrlen = sizeof(address);
    char buffer[BUFFER_SIZE] = {0};

    // Cloud hosts (like Render) assign a random port via the PORT environment variable.
    // If it's not set, we default to 8080 for local testing.
    char *port_env = getenv("PORT");
    int PORT = port_env ? atoi(port_env) : 8080;

    // 1. Create Socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Socket failed"); exit(EXIT_FAILURE);
    }

    // Allow port reuse
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    // 2. Bind and Listen
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Bind failed"); exit(EXIT_FAILURE);
    }
    if (listen(server_fd, 3) < 0) {
        perror("Listen failed"); exit(EXIT_FAILURE);
    }

    printf("🚀 Voting Server is running on port %d!\n", PORT);

    // 3. Accept Connections and Handle Requests
    while (1) {
        if ((new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
            perror("Accept failed"); continue;
        }

        read(new_socket, buffer, BUFFER_SIZE - 1);

        char response_body[BUFFER_SIZE * 2];

        // Route: GET / (Homepage)
        if (strncmp(buffer, "GET /", 5) == 0 && strncmp(buffer, "GET /vote", 9) != 0) {
            generate_homepage(response_body);
            send_response(new_socket, 200, "OK", response_body);
        } 
        // Route: POST /vote (Handle Voting)
        else if (strncmp(buffer, "POST /vote", 10) == 0) {
            // Parse the form data from the HTTP body
            char *body_start = strstr(buffer, "\r\n\r\n");
            if (body_start) {
                body_start += 4; // Skip the \r\n\r\n
                char *candidate_str = strstr(body_start, "candidate=");
                if (candidate_str) {
                    int id = atoi(candidate_str + 10); // Extract the number
                    if (id >= 0 && id < 3) {
                        candidates[id].votes++;
                        printf("🗳️ Vote cast for: %s (Total: %d)\n", candidates[id].name, candidates[id].votes);
                    }
                }
            }
            generate_success_page(response_body);
            send_response(new_socket, 200, "OK", response_body);
        } 
        // Route: 404 Not Found
        else {
            send_response(new_socket, 404, "Not Found", "<h1>404 - Page Not Found</h1>");
        }

        close(new_socket);
        memset(buffer, 0, BUFFER_SIZE); // Clear buffer for next request
    }

    return 0;
}