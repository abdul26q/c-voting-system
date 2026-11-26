#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <time.h>

#define BUFFER_SIZE 65536
#define MAX_ELECTIONS 10
#define MAX_CANDIDATES 50
#define MAX_VOTERS 1000
#define ADMIN_PASSWORD "admin123"

// Data Structures
typedef struct {
    char name[100];
    char party[100];
    int votes;
} Candidate;

typedef struct {
    int id;
    char title[200];
    char description[500];
    int active;
    int candidate_count;
    Candidate candidates[MAX_CANDIDATES];
    int voter_count;
    char voter_ids[MAX_VOTERS][50];
} Election;

Election elections[MAX_ELECTIONS];
int election_count = 0;

// Static buffers to avoid stack bloat/corruption
static char g_page_buf[BUFFER_SIZE];
static char g_body_buf[BUFFER_SIZE];
static char g_list_buf[16384];
static char g_opts_buf[4096];
static char g_sock_buf[BUFFER_SIZE];

// Helper: Generate unique election ID
int generate_election_id() {
    srand((unsigned int)(time(NULL) + election_count * 7919));
    int id = rand() % 90000 + 10000;
    for (int i = 0; i < election_count; i++) {
        if (elections[i].id == id) {
            id = 10000 + ((id + 1) % 89999);
        }
    }
    return id;
}

// Helper: URL decode
void url_decode(char *dst, const char *src) {
    char a, b;
    while (*src) {
        if ((*src == '%') && ((a = src[1]) && (b = src[2])) &&
            (isxdigit((unsigned char)a) && isxdigit((unsigned char)b))) {
            if (a >= 'a') a = (char)(a - 'a' + 'A');
            if (a >= 'A') a = (char)(a - 'A' + 10); else a = (char)(a - '0');
            if (b >= 'a') b = (char)(b - 'a' + 'A');
            if (b >= 'A') b = (char)(b - 'A' + 10); else b = (char)(b - '0');
            *dst++ = (char)(16 * a + b);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

// Helper: Extract value from URL-encoded form data
char* get_form_value(const char *data, const char *key, char *value, int max_len) {
    size_t key_len = strlen(key);
    const char *p = data;

    while (*p) {
        int at_boundary = (p == data) || (*(p - 1) == '&');
        if (at_boundary && strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            const char *val_start = p + key_len + 1;
            int i = 0;
            while (val_start[i] && val_start[i] != '&' && i < max_len - 1) {
                value[i] = val_start[i];
                i++;
            }
            value[i] = '\0';
            url_decode(value, value);
            return value;
        }
        const char *amp = strchr(p, '&');
        if (!amp) break;
        p = amp + 1;
    }
    value[0] = '\0';
    return NULL;
}

// Helper: Send HTTP response
void send_response(int client, int status, const char *status_text, const char *body) {
    char header[2048];
    int body_len = (int)strlen(body);
    snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/html; charset=UTF-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n",
        status, status_text, body_len);
    send(client, header, strlen(header), 0);
    send(client, body, (size_t)body_len, 0);
}

void send_redirect(int client, const char *location) {
    char response[1024];
    snprintf(response, sizeof(response),
        "HTTP/1.1 302 Found\r\nLocation: %s\r\nConnection: close\r\n\r\n",
        location);
    send(client, response, strlen(response), 0);
}

// Shared CSS & Head
const char *HEAD =
    "<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>VotePro | Shape the Future</title>"
    "<script src='https://cdn.tailwindcss.com'></script>"
    "<link href='https://fonts.googleapis.com/css2?family=Inter:wght@300;400;500;600;700&display=swap' rel='stylesheet'>"
    "<style>"
    "* { font-family: 'Inter', sans-serif; }"
    "body { background: #000; color: #fff; overflow-x: hidden; }"
    ".video-bg { position: fixed; inset: 0; z-index: 0; }"
    ".video-bg video { width: 100%; height: 100%; object-fit: cover; }"
    ".overlay { position: fixed; inset: 0; z-index: 1; background: rgba(0,0,0,0.6); }"
    ".content { position: relative; z-index: 10; }"
    ".shiny-text {"
    "  background: linear-gradient(100deg, #64CEFB 0%, #64CEFB 30%, #ffffff 50%, #64CEFB 70%, #64CEFB 100%);"
    "  background-size: 200% 100%;"
    "  -webkit-background-clip: text; background-clip: text;"
    "  color: transparent;"
    "  animation: shine 3s linear infinite;"
    "}"
    "@keyframes shine { 0% { background-position: 100% 50%; } 100% { background-position: 0% 50%; } }"
    ".glass { background: rgba(255,255,255,0.05); backdrop-filter: blur(10px); border: 1px solid rgba(255,255,255,0.1); }"
    ".nav-pill { background: rgba(55,65,81,0.5); border: 1px solid rgba(75,85,99,0.5); }"
    "</style></head><body>";

const char *FOOT = "</body></html>";

void build_page(char *buf, const char *body) {
    snprintf(buf, BUFFER_SIZE, "%s%s%s", HEAD, body, FOOT);
}

// Landing Page
void page_landing(char *buf, const char *base_url) {
    g_list_buf[0] = '\0';
    char temp[2048];

    for (int i = 0; i < election_count; i++) {
        snprintf(temp, sizeof(temp),
            "<div class='glass rounded-2xl p-8 hover:bg-white/10 transition-all duration-300 group'>"
            "<div class='flex justify-between items-start mb-4'>"
            "<h3 class='text-2xl font-semibold text-white'>%s</h3>"
            "<span class='px-3 py-1 rounded-full text-xs font-medium %s'>%s</span>"
            "</div>"
            "<p class='text-white/70 mb-2'>%s</p>"
            "<p class='text-white/40 text-xs mb-6'>Election ID: %d • Candidates: %d</p>"
            "<div class='flex gap-3'>"
            "<a href='%s/vote/%d' class='flex-1 bg-white text-black text-center py-3 rounded-full font-medium hover:bg-gray-200 transition'>🗳️ Vote Now</a>"
            "<a href='%s/results/%d' class='flex-1 glass text-white text-center py-3 rounded-full font-medium hover:bg-white/20 transition'>📊 Results</a>"
            "</div></div>",
            elections[i].title,
            elections[i].active ? "bg-green-500/20 text-green-400" : "bg-red-500/20 text-red-400",
            elections[i].active ? "ACTIVE" : "CLOSED",
            elections[i].description,
            elections[i].id, elections[i].candidate_count,
            base_url, elections[i].id,
            base_url, elections[i].id);
        strncat(g_list_buf, temp, sizeof(g_list_buf) - strlen(g_list_buf) - 1);
    }

    if (election_count == 0) {
        strcpy(g_list_buf, "<div class='col-span-2 text-center py-20 text-white/50'>No active elections yet. Check back soon.</div>");
    }

    snprintf(g_body_buf, sizeof(g_body_buf),
        "<div class='video-bg'><video autoplay loop muted playsinline><source src='https://d8j0ntlcm91z4.cloudfront.net/user_38xzZboKViGWJOttwIXH07lWA1P/hf_20260328_105406_16f4600d-7a92-4292-b96e-b19156c7830a.mp4' type='video/mp4'></video></div>"
        "<div class='overlay'></div>"
        "<div class='content min-h-screen flex flex-col'>"
        "<nav class='w-full px-4 sm:px-6 lg:px-12 py-4 sm:py-6'><div class='max-w-7xl mx-auto flex flex-wrap justify-between items-center gap-3'>"
        "<div class='flex items-center gap-3'><div class='w-9 h-9 sm:w-10 sm:h-10 rounded-full border-2 border-white flex items-center justify-center'><div class='w-4 h-4 sm:w-5 sm:h-5 rounded-full bg-white'></div></div><span class='text-lg sm:text-xl font-semibold'>VotePro</span></div>"
        "<div class='flex flex-wrap justify-end items-center gap-1 sm:gap-2 nav-pill rounded-full px-2 py-1.5 sm:px-2 sm:py-2'>"
        "<a href='%s/' class='px-3 py-1.5 sm:px-4 sm:py-2 text-xs sm:text-sm text-white/80 hover:text-white rounded-full hover:bg-white/10 transition'>Home</a>"
        "<a href='%s/' class='px-3 py-1.5 sm:px-4 sm:py-2 text-xs sm:text-sm text-white/80 hover:text-white rounded-full hover:bg-white/10 transition'>Elections</a>"
        "<a href='%s/admin' class='px-3 py-1.5 sm:px-4 sm:py-2 text-xs sm:text-sm text-white/80 hover:text-white rounded-full hover:bg-white/10 transition'>Admin</a>"
        "</div></div></nav>"
        "<section class='flex-1 flex items-center justify-center px-6 py-16 sm:py-20'><div class='max-w-7xl mx-auto w-full'>"
        "<div class='text-center mb-16'><p class='text-xs sm:text-sm text-white/80 uppercase tracking-tight mb-6'>Democracy in Action</p>"
        "<h1 class='text-5xl sm:text-6xl md:text-7xl lg:text-8xl font-medium leading-[0.85] tracking-tighter'>"
        "<span class='block text-white'>Shape the</span>"
        "<span class='block shiny-text'>Future Today.</span></h1>"
        "<p class='mt-8 text-white/70 max-w-2xl mx-auto text-lg'>Participate in secure, transparent elections. Your vote matters.</p></div>"
        "<div class='grid grid-cols-1 lg:grid-cols-2 gap-6'>%s"
        "</div></div></section>"
        "<footer class='text-center py-8 text-white/40 text-sm'>Built with C Sockets | No frameworks</footer>"
        "</div>",
        base_url, base_url, base_url, g_list_buf);

    build_page(buf, g_body_buf);
}

// Admin Login
void page_admin_login(char *buf, const char *base_url, const char *error) {
    snprintf(g_body_buf, sizeof(g_body_buf),
        "<div class='video-bg'><video autoplay loop muted playsinline><source src='https://d8j0ntlcm91z4.cloudfront.net/user_38xzZboKViGWJOttwIXH07lWA1P/hf_20260328_105406_16f4600d-7a92-4292-b96e-b19156c7830a.mp4' type='video/mp4'></video></div>"
        "<div class='overlay'></div>"
        "<div class='content min-h-screen flex items-center justify-center px-6'>"
        "<div class='glass rounded-2xl p-8 w-full max-w-md'>"
        "<div class='text-center mb-8'><div class='w-12 h-12 rounded-full border-2 border-white flex items-center justify-center mx-auto mb-4'><div class='w-6 h-6 rounded-full bg-white'></div></div>"
        "<h1 class='text-2xl font-semibold'>Admin Access</h1></div>"
        "%s"
        "<form method='POST' action='%s/admin/login'>"
        "<label class='block text-sm text-white/70 mb-2'>Password</label>"
        "<input type='password' name='password' required class='w-full bg-white/5 border border-white/10 rounded-lg px-4 py-3 text-white focus:outline-none focus:border-blue-400 transition'>"
        "<button type='submit' class='w-full bg-white text-black font-medium py-3 rounded-full mt-6 hover:bg-gray-200 transition'>Login</button>"
        "</form>"
        "<p class='text-center mt-6'><a href='%s/' class='text-white/60 hover:text-white text-sm'>← Back to Home</a></p>"
        "</div></div>",
        error[0] ? "<div class='bg-red-500/20 border border-red-500/50 text-red-400 px-4 py-3 rounded-lg mb-6 text-sm'>❌ Invalid password</div>" : "",
        base_url, base_url);
    build_page(buf, g_body_buf);
}

// Admin Dashboard
void page_admin_dashboard(char *buf, const char *base_url) {
    g_list_buf[0] = '\0';
    char temp[2048];
    for (int i = 0; i < election_count; i++) {
        snprintf(temp, sizeof(temp),
            "<div class='glass rounded-xl p-6'>"
            "<div class='flex justify-between items-start mb-4'><h3 class='text-lg font-semibold text-white'>%s</h3>"
            "<span class='text-xs px-2 py-1 rounded-full %s'>%s</span></div>"
            "<p class='text-white/60 text-sm mb-4'>ID: %d | Candidates: %d | Voters: %d</p>"
            "<div class='flex gap-2'><a href='%s/results/%d' class='flex-1 text-center py-2 rounded-lg bg-white/10 text-sm hover:bg-white/20 transition'>Results</a>"
            "<a href='%s/admin/election/%d/toggle' class='flex-1 text-center py-2 rounded-lg %s text-sm transition'>%s</a></div></div>",
            elections[i].title,
            elections[i].active ? "bg-green-500/20 text-green-400" : "bg-red-500/20 text-red-400",
            elections[i].active ? "ACTIVE" : "CLOSED",
            elections[i].id, elections[i].candidate_count, elections[i].voter_count,
            base_url, elections[i].id,
            base_url, elections[i].id,
            elections[i].active ? "bg-red-500/20 text-red-400 hover:bg-red-500/30" : "bg-green-500/20 text-green-400 hover:bg-green-500/30",
            elections[i].active ? "Deactivate" : "Activate");
        strncat(g_list_buf, temp, sizeof(g_list_buf) - strlen(g_list_buf) - 1);
    }

    g_opts_buf[0] = '\0';
    char opt[512];
    for (int i = 0; i < election_count; i++) {
        snprintf(opt, sizeof(opt), "<option value='%d' class='bg-gray-900 text-white'>%s (ID: %d)</option>", elections[i].id, elections[i].title, elections[i].id);
        strncat(g_opts_buf, opt, sizeof(g_opts_buf) - strlen(g_opts_buf) - 1);
    }

    snprintf(g_body_buf, sizeof(g_body_buf),
        "<div class='video-bg'><video autoplay loop muted playsinline><source src='https://d8j0ntlcm91z4.cloudfront.net/user_38xzZboKViGWJOttwIXH07lWA1P/hf_20260328_105406_16f4600d-7a92-4292-b96e-b19156c7830a.mp4' type='video/mp4'></video></div>"
        "<div class='overlay'></div>"
        "<div class='content min-h-screen px-6 py-12'><div class='max-w-7xl mx-auto'>"
        "<nav class='flex flex-wrap justify-between items-center gap-3 mb-12'><div class='flex items-center gap-3'><div class='w-10 h-10 rounded-full border-2 border-white flex items-center justify-center'><div class='w-5 h-5 rounded-full bg-white'></div></div><span class='text-xl font-semibold'>VotePro Admin</span></div>"
        "<div class='flex gap-3'><a href='%s/' class='px-5 py-2 rounded-full bg-white/10 text-white hover:bg-white/20 transition text-sm'>Home</a>"
        "<a href='%s/admin/logout' class='px-6 py-2 rounded-full bg-red-500/20 text-red-400 hover:bg-red-500/30 transition text-sm'>Logout</a></div></nav>"
        "<div class='grid grid-cols-1 lg:grid-cols-2 gap-8 mb-12'>"
        "<div class='glass rounded-2xl p-8'><h2 class='text-2xl font-semibold mb-6'>➕ Create Election</h2>"
        "<form method='POST' action='%s/admin/election/create'>"
        "<label class='block text-sm text-white/70 mb-2'>Title</label>"
        "<input type='text' name='title' required class='w-full bg-white/5 border border-white/10 rounded-lg px-4 py-3 text-white mb-4 focus:outline-none focus:border-blue-400'>"
        "<label class='block text-sm text-white/70 mb-2'>Description</label>"
        "<textarea name='description' rows='3' required class='w-full bg-white/5 border border-white/10 rounded-lg px-4 py-3 text-white mb-6 focus:outline-none focus:border-blue-400'></textarea>"
        "<button type='submit' class='w-full bg-white text-black font-medium py-3 rounded-full hover:bg-gray-200 transition'>Create Election</button>"
        "</form></div>"
        "<div class='glass rounded-2xl p-8'><h2 class='text-2xl font-semibold mb-6'>➕ Add Candidate</h2>"
        "<form method='POST' action='%s/admin/candidate/add'>"
        "<label class='block text-sm text-white/70 mb-2'>Election</label>"
        "<select name='election_id' required class='w-full bg-white/5 border border-white/10 rounded-lg px-4 py-3 text-white mb-4 focus:outline-none focus:border-blue-400'>%s</select>"
        "<label class='block text-sm text-white/70 mb-2'>Candidate Name</label>"
        "<input type='text' name='name' required class='w-full bg-white/5 border border-white/10 rounded-lg px-4 py-3 text-white mb-4 focus:outline-none focus:border-blue-400'>"
        "<label class='block text-sm text-white/70 mb-2'>Party</label>"
        "<input type='text' name='party' required class='w-full bg-white/5 border border-white/10 rounded-lg px-4 py-3 text-white mb-6 focus:outline-none focus:border-blue-400'>"
        "<button type='submit' class='w-full bg-white text-black font-medium py-3 rounded-full hover:bg-gray-200 transition'>Add Candidate</button>"
        "</form></div></div>"
        "<h2 class='text-2xl font-semibold mb-6'>📋 Manage Elections</h2>"
        "<div class='grid grid-cols-1 md:grid-cols-2 lg:grid-cols-3 gap-6'>%s"
        "</div></div></div>",
        base_url, base_url, base_url, base_url, g_opts_buf, g_list_buf);
    build_page(buf, g_body_buf);
}

// Voting Page
void page_vote(char *buf, const char *base_url, int election_id, const char *error) {
    Election *elec = NULL;
    for (int i = 0; i < election_count; i++) {
        if (elections[i].id == election_id) {
            elec = &elections[i];
            break;
        }
    }

    if (!elec || !elec->active) {
        snprintf(g_body_buf, sizeof(g_body_buf), "<div class='content min-h-screen flex items-center justify-center px-6'><div class='glass rounded-2xl p-12 text-center max-w-md'><h1 class='text-3xl font-semibold mb-4'>❌ Election Unavailable</h1><p class='text-white/70 mb-8'>This election is inactive or doesn't exist.</p><a href='%s/' class='px-8 py-3 rounded-full bg-white text-black font-medium hover:bg-gray-200 transition'>Back to Home</a></div></div>", base_url);
        build_page(buf, g_body_buf);
        return;
    }

    g_list_buf[0] = '\0';
    char temp[1024];
    for (int i = 0; i < elec->candidate_count; i++) {
        snprintf(temp, sizeof(temp),
            "<label class='glass rounded-xl p-6 flex items-center gap-4 cursor-pointer hover:bg-white/10 transition border-2 border-transparent has-[:checked]:border-blue-400 has-[:checked]:bg-blue-500/10'>"
            "<input type='radio' name='candidate_id' value='%d' required class='w-5 h-5 accent-blue-400'>"
            "<div><div class='font-semibold text-lg'>%s</div><div class='text-white/60 text-sm'>%s</div></div></label>",
            i, elec->candidates[i].name, elec->candidates[i].party);
        strncat(g_list_buf, temp, sizeof(g_list_buf) - strlen(g_list_buf) - 1);
    }

    char error_html[512] = "";
    if (error[0]) {
        snprintf(error_html, sizeof(error_html), "<div class='bg-red-500/20 border border-red-500/50 text-red-400 px-4 py-3 rounded-lg mb-6 text-sm text-center'>❌ %s</div>", error);
    }

    snprintf(g_body_buf, sizeof(g_body_buf),
        "<div class='video-bg'><video autoplay loop muted playsinline><source src='https://d8j0ntlcm91z4.cloudfront.net/user_38xzZboKViGWJOttwIXH07lWA1P/hf_20260328_105406_16f4600d-7a92-4292-b96e-b19156c7830a.mp4' type='video/mp4'></video></div>"
        "<div class='overlay'></div>"
        "<div class='content min-h-screen flex items-center justify-center px-6 py-12'>"
        "<div class='w-full max-w-2xl'>"
        "<div class='text-center mb-10'><p class='text-xs sm:text-sm text-white/80 uppercase tracking-tight mb-4'>Official Ballot • ID %d</p>"
        "<h1 class='text-4xl sm:text-5xl font-semibold mb-4'>%s</h1>"
        "<p class='text-white/70'>%s</p></div>"
        "%s"
        "<form method='POST' action='%s/vote/%d' class='glass rounded-2xl p-8'>"
        "<label class='block text-sm text-white/70 mb-2'>Your Voter ID (Student ID / Email)</label>"
        "<input type='text' name='voter_id' required placeholder='Enter your unique ID' class='w-full bg-white/5 border border-white/10 rounded-lg px-4 py-3 text-white mb-6 focus:outline-none focus:border-blue-400'>"
        "<div class='space-y-3 mb-8'>%s</div>"
        "<button type='submit' class='w-full bg-white text-black font-medium py-4 rounded-full hover:bg-gray-200 transition text-lg'>✅ Cast My Vote</button>"
        "</form>"
        "<div class='text-center mt-8'><a href='%s/results/%d' class='text-white/60 hover:text-white text-sm'>📊 View Live Results</a></div>"
        "</div></div>",
        elec->id, elec->title, elec->description, error_html,
        base_url, election_id, g_list_buf, base_url, election_id);

    build_page(buf, g_body_buf);
}

// Results Page
void page_results(char *buf, const char *base_url, int election_id) {
    Election *elec = NULL;
    for (int i = 0; i < election_count; i++) {
        if (elections[i].id == election_id) {
            elec = &elections[i];
            break;
        }
    }
    if (!elec) {
        snprintf(g_body_buf, sizeof(g_body_buf), "<div class='content min-h-screen flex items-center justify-center px-6'><div class='glass rounded-2xl p-12 text-center max-w-md'><h1 class='text-3xl font-semibold mb-4'>Election Not Found</h1><a href='%s/' class='px-8 py-3 rounded-full bg-white text-black font-medium'>Back to Home</a></div></div>", base_url);
        build_page(buf, g_body_buf);
        return;
    }

    int total_votes = 0;
    for (int i = 0; i < elec->candidate_count; i++) total_votes += elec->candidates[i].votes;

    g_list_buf[0] = '\0';
    char temp[1024];
    for (int i = 0; i < elec->candidate_count; i++) {
        int pct = total_votes > 0 ? (elec->candidates[i].votes * 100 / total_votes) : 0;
        snprintf(temp, sizeof(temp),
            "<div class='mb-6'><div class='flex justify-between mb-2'><span class='font-semibold'>%s</span><span class='text-white/60 text-sm'>%s</span><span class='font-bold text-blue-400'>%d%%</span></div>"
            "<div class='w-full bg-white/10 rounded-full h-3'><div class='bg-gradient-to-r from-blue-400 to-cyan-300 h-3 rounded-full transition-all duration-1000' style='width:%d%%'></div></div>"
            "<div class='text-right text-xs text-white/50 mt-1'>%d votes</div></div>",
            elec->candidates[i].name, elec->candidates[i].party, pct, pct, elec->candidates[i].votes);
        strncat(g_list_buf, temp, sizeof(g_list_buf) - strlen(g_list_buf) - 1);
    }

    snprintf(g_body_buf, sizeof(g_body_buf),
        "<div class='video-bg'><video autoplay loop muted playsinline><source src='https://d8j0ntlcm91z4.cloudfront.net/user_38xzZboKViGWJOttwIXH07lWA1P/hf_20260328_105406_16f4600d-7a92-4292-b96e-b19156c7830a.mp4' type='video/mp4'></video></div>"
        "<div class='overlay'></div>"
        "<div class='content min-h-screen px-6 py-12'><div class='max-w-3xl mx-auto'>"
        "<div class='text-center mb-12'><p class='text-xs sm:text-sm text-white/80 uppercase tracking-tight mb-4'>Live Results • Election ID %d</p>"
        "<h1 class='text-4xl sm:text-5xl font-semibold mb-4'>%s</h1>"
        "<div class='flex justify-center gap-6 mt-8'><div class='glass rounded-xl px-6 py-4'><div class='text-3xl font-bold text-blue-400'>%d</div><div class='text-xs text-white/60 uppercase tracking-wider mt-1'>Total Votes</div></div>"
        "<div class='glass rounded-xl px-6 py-4'><div class='text-3xl font-bold text-cyan-400'>%d</div><div class='text-xs text-white/60 uppercase tracking-wider mt-1'>Voters</div></div></div></div>"
        "<div class='glass rounded-2xl p-8 mb-8'>%s</div>"
        "<div class='text-center'><a href='%s/vote/%d' class='px-8 py-3 rounded-full bg-white text-black font-medium hover:bg-gray-200 transition'>🗳️ Vote Now</a>"
        "<a href='%s/' class='px-8 py-3 rounded-full glass text-white font-medium hover:bg-white/20 transition ml-4'>← Home</a></div>"
        "</div></div>",
        elec->id, elec->title, total_votes, elec->voter_count, g_list_buf, base_url, election_id, base_url);
    build_page(buf, g_body_buf);
}

// Success Page
void page_success(char *buf, const char *base_url, int election_id) {
    snprintf(g_body_buf, sizeof(g_body_buf),
        "<div class='video-bg'><video autoplay loop muted playsinline><source src='https://d8j0ntlcm91z4.cloudfront.net/user_38xzZboKViGWJOttwIXH07lWA1P/hf_20260328_105406_16f4600d-7a92-4292-b96e-b19156c7830a.mp4' type='video/mp4'></video></div>"
        "<div class='overlay'></div>"
        "<div class='content min-h-screen flex items-center justify-center px-6'>"
        "<div class='glass rounded-2xl p-12 text-center max-w-md'>"
        "<div class='w-20 h-20 rounded-full bg-green-500/20 flex items-center justify-center mx-auto mb-6'><svg class='w-10 h-10 text-green-400' fill='none' stroke='currentColor' viewBox='0 0 24 24'><path stroke-linecap='round' stroke-linejoin='round' stroke-width='2' d='M5 13l4 4L19 7'></path></svg></div>"
        "<h1 class='text-3xl font-semibold mb-4'>Vote Recorded!</h1>"
        "<p class='text-white/70 mb-8'>Thank you for participating. Your vote has been securely cast.</p>"
        "<div class='flex gap-4'><a href='%s/results/%d' class='flex-1 py-3 rounded-full bg-white text-black font-medium hover:bg-gray-200 transition'>View Results</a>"
        "<a href='%s/' class='flex-1 py-3 rounded-full glass text-white font-medium hover:bg-white/20 transition'>Home</a></div>"
        "</div></div>", base_url, election_id, base_url);
    build_page(buf, g_body_buf);
}

void parse_path(const char *request, char *path, int max_len) {
    const char *start = strstr(request, " ");
    if (!start) { path[0] = '\0'; return; }
    start++;
    const char *end = strstr(start, " ");
    if (!end) { path[0] = '\0'; return; }
    int len = (int)(end - start);
    if (len >= max_len) len = max_len - 1;
    strncpy(path, start, (size_t)len);
    path[len] = '\0';
}

int voter_already_voted(Election *elec, const char *voter_id) {
    for (int i = 0; i < elec->voter_count; i++) {
        if (strcmp(elec->voter_ids[i], voter_id) == 0) return 1;
    }
    return 0;
}

int main() {
    int server_fd, new_socket;
    struct sockaddr_in address;
    socklen_t addrlen = sizeof(address);

    char *port_env = getenv("PORT");
    int PORT = port_env ? atoi(port_env) : 8080;

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) { perror("Socket failed"); exit(EXIT_FAILURE); }
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons((uint16_t)PORT);
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) { perror("Bind failed"); exit(EXIT_FAILURE); }
    if (listen(server_fd, 10) < 0) { perror("Listen failed"); exit(EXIT_FAILURE); }

    printf("🚀 VotePro Server running on port %d!\n", PORT);

    while (1) {
        if ((new_socket = accept(server_fd, (struct sockaddr *)&address, &addrlen)) < 0) { perror("Accept failed"); continue; }
        memset(g_sock_buf, 0, BUFFER_SIZE);

        ssize_t bytes_read = read(new_socket, g_sock_buf, BUFFER_SIZE - 1);
        if (bytes_read <= 0) {
            close(new_socket);
            continue;
        }
        g_sock_buf[bytes_read] = '\0';

        if (strncmp(g_sock_buf, "POST ", 5) == 0) {
            char *cl_hdr = strcasestr(g_sock_buf, "Content-Length:");
            char *body_start = strstr(g_sock_buf, "\r\n\r\n");
            if (cl_hdr && body_start) {
                int content_length = atoi(cl_hdr + 15);
                body_start += 4;
                int body_received = (int)(bytes_read - (body_start - g_sock_buf));
                while (body_received < content_length && bytes_read < BUFFER_SIZE - 1) {
                    ssize_t more = read(new_socket, g_sock_buf + bytes_read, (size_t)(BUFFER_SIZE - 1 - bytes_read));
                    if (more <= 0) break;
                    bytes_read += more;
                    body_received += (int)more;
                    g_sock_buf[bytes_read] = '\0';
                }
            }
        }

        char path[512] = {0};
        parse_path(g_sock_buf, path, sizeof(path));
        const char *base_url = "";

        if (strncmp(g_sock_buf, "GET / ", 6) == 0 || strncmp(g_sock_buf, "GET / HTTP", 10) == 0) {
            page_landing(g_page_buf, base_url);
            send_response(new_socket, 200, "OK", g_page_buf);
        }
        else if (strncmp(g_sock_buf, "GET /admin ", 11) == 0 || strncmp(g_sock_buf, "GET /admin HTTP", 15) == 0) {
            page_admin_login(g_page_buf, base_url, "");
            send_response(new_socket, 200, "OK", g_page_buf);
        }
        else if (strncmp(g_sock_buf, "POST /admin/login", 17) == 0) {
            char *body_start = strstr(g_sock_buf, "\r\n\r\n");
            char password[100] = {0};
            if (body_start) get_form_value(body_start + 4, "password", password, sizeof(password));
            if (strcmp(password, ADMIN_PASSWORD) == 0) send_redirect(new_socket, "/admin/dashboard");
            else { page_admin_login(g_page_buf, base_url, "Invalid password"); send_response(new_socket, 200, "OK", g_page_buf); }
        }
        else if (strncmp(g_sock_buf, "GET /admin/dashboard", 20) == 0) {
            page_admin_dashboard(g_page_buf, base_url);
            send_response(new_socket, 200, "OK", g_page_buf);
        }
        else if (strncmp(g_sock_buf, "POST /admin/election/create", 27) == 0) {
            char *body_start = strstr(g_sock_buf, "\r\n\r\n");
            if (body_start && election_count < MAX_ELECTIONS) {
                body_start += 4;
                Election *elec = &elections[election_count];
                elec->id = generate_election_id();
                elec->active = 1;
                elec->candidate_count = 0;
                elec->voter_count = 0;
                get_form_value(body_start, "title", elec->title, sizeof(elec->title));
                get_form_value(body_start, "description", elec->description, sizeof(elec->description));
                if (elec->title[0] != '\0') {
                    election_count++;
                    printf("✅ Created election: %s (ID: %d)\n", elec->title, elec->id);
                }
            }
            send_redirect(new_socket, "/admin/dashboard");
        }
        else if (strncmp(g_sock_buf, "GET /admin/election/", 20) == 0 && strstr(g_sock_buf, "/toggle")) {
            char id_str[20] = {0};
            sscanf(path, "/admin/election/%19[^/]/toggle", id_str);
            int id = atoi(id_str);
            for (int i = 0; i < election_count; i++) {
                if (elections[i].id == id) {
                    elections[i].active = !elections[i].active;
                    break;
                }
            }
            send_redirect(new_socket, "/admin/dashboard");
        }
        else if (strncmp(g_sock_buf, "POST /admin/candidate/add", 25) == 0) {
            char *body_start = strstr(g_sock_buf, "\r\n\r\n");
            if (body_start) {
                body_start += 4;
                char elec_id_str[20] = {0};
                get_form_value(body_start, "election_id", elec_id_str, sizeof(elec_id_str));
                int elec_id = atoi(elec_id_str);
                for (int i = 0; i < election_count; i++) {
                    if (elections[i].id == elec_id && elections[i].candidate_count < MAX_CANDIDATES) {
                        Candidate *cand = &elections[i].candidates[elections[i].candidate_count];
                        cand->votes = 0;
                        get_form_value(body_start, "name", cand->name, sizeof(cand->name));
                        get_form_value(body_start, "party", cand->party, sizeof(cand->party));
                        if (cand->name[0] != '\0') {
                            elections[i].candidate_count++;
                            printf("✅ Added candidate '%s' (%s) to election '%s' (ID: %d), total candidates: %d\n",
                                   cand->name, cand->party, elections[i].title, elections[i].id, elections[i].candidate_count);
                        }
                        break;
                    }
                }
            }
            send_redirect(new_socket, "/admin/dashboard");
        }
        else if (strncmp(g_sock_buf, "GET /vote/", 10) == 0) {
            char id_str[20] = {0};
            sscanf(path, "/vote/%19[^/]", id_str);
            page_vote(g_page_buf, base_url, atoi(id_str), "");
            send_response(new_socket, 200, "OK", g_page_buf);
        }
        else if (strncmp(g_sock_buf, "POST /vote/", 11) == 0) {
            char id_str[20] = {0};
            sscanf(path, "/vote/%19[^/]", id_str);
            int id = atoi(id_str);
            char *body_start = strstr(g_sock_buf, "\r\n\r\n");
            char voter_id[100] = {0}, cand_id_str[20] = {0};
            if (body_start) {
                body_start += 4;
                get_form_value(body_start, "voter_id", voter_id, sizeof(voter_id));
                get_form_value(body_start, "candidate_id", cand_id_str, sizeof(cand_id_str));
            }
            int cand_id = atoi(cand_id_str);
            int voted = 0;
            for (int i = 0; i < election_count; i++) {
                if (elections[i].id == id && elections[i].active) {
                    if (voter_id[0] == '\0') {
                        page_vote(g_page_buf, base_url, id, "Please enter a valid Voter ID!");
                        send_response(new_socket, 200, "OK", g_page_buf);
                        voted = 1;
                        break;
                    }
                    if (voter_already_voted(&elections[i], voter_id)) {
                        page_vote(g_page_buf, base_url, id, "You have already voted!");
                        send_response(new_socket, 200, "OK", g_page_buf);
                        voted = 1;
                        break;
                    }
                    if (cand_id >= 0 && cand_id < elections[i].candidate_count && elections[i].voter_count < MAX_VOTERS) {
                        elections[i].candidates[cand_id].votes++;
                        strncpy(elections[i].voter_ids[elections[i].voter_count], voter_id, sizeof(elections[i].voter_ids[0]) - 1);
                        elections[i].voter_ids[elections[i].voter_count][sizeof(elections[i].voter_ids[0]) - 1] = '\0';
                        elections[i].voter_count++;
                        printf("🗳️ %s voted for %s in election %d\n", voter_id, elections[i].candidates[cand_id].name, elections[i].id);
                        page_success(g_page_buf, base_url, id);
                        send_response(new_socket, 200, "OK", g_page_buf);
                        voted = 1;
                    }
                    break;
                }
            }
            if (!voted) {
                page_vote(g_page_buf, base_url, id, "Invalid election or candidate!");
                send_response(new_socket, 200, "OK", g_page_buf);
            }
        }
        else if (strncmp(g_sock_buf, "GET /results/", 13) == 0) {
            char id_str[20] = {0};
            sscanf(path, "/results/%19[^/]", id_str);
            page_results(g_page_buf, base_url, atoi(id_str));
            send_response(new_socket, 200, "OK", g_page_buf);
        }
        else if (strncmp(g_sock_buf, "GET /admin/logout", 17) == 0) {
            send_redirect(new_socket, "/");
        }
        else {
            char body[2048];
            snprintf(body, sizeof(body), "<div class='content min-h-screen flex items-center justify-center'><h1 class='text-4xl font-semibold'>404</h1><a href='%s/' class='mt-4 px-6 py-2 rounded-full bg-white text-black'>Home</a></div>", base_url);
            build_page(g_page_buf, body);
            send_response(new_socket, 404, "Not Found", g_page_buf);
        }
        close(new_socket);
    }
    return 0;
}
