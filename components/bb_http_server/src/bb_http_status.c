#include "bb_http_status.h"
#include <stddef.h>

const char *bb_http_status_reason(int status_code)
{
    switch (status_code) {
        case 200: return "200 OK";
        case 201: return "201 Created";
        case 202: return "202 Accepted";
        case 204: return "204 No Content";
        case 302: return "302 Found";
        case 400: return "400 Bad Request";
        case 401: return "401 Unauthorized";
        case 403: return "403 Forbidden";
        case 404: return "404 Not Found";
        case 408: return "408 Request Timeout";
        case 409: return "409 Conflict";
        case 412: return "412 Precondition Failed";
        case 422: return "422 Unprocessable Entity";
        case 500: return "500 Internal Server Error";
        case 503: return "503 Service Unavailable";
        default:  return NULL;
    }
}
