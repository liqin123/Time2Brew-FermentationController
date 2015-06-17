#include <p33Exxxx.h>
#include <stddef.h>
#include <stdlib.h>
#include "circularPrintF.h"
#include <string.h>
#include <stdio.h>
#include "ESP8266.h"
#include "integer.h"
#include "Base64.h"
#include "http_server.h"
#include "SystemConfiguration.h"
#include "ESP8266.h"
#include "Http_API.h"
#include "MD5.h"
#include "FlashFS.h"
#include "BlobFS.h"
#include "uPnP.h"

#define REQUEST_EXPIRATION_TIME 10
#define NONCE_LIFETIME  5000UL
#define DIGEST_CONNECTIONS_MAX 10

typedef struct {
    int isValid;
    char nonce[34];
    unsigned long expires;
} DIGEST_CTX;

DIGEST_CTX Digest_CTXs[DIGEST_CONNECTIONS_MAX];

//char FileETag[32];
//BYTE fileBuffer[1536];

HTTP_REQUEST HTTP_Channels[HTTP_CHANNEL_COUNT];

const char *defaultResource = "/index.html";

int HexToDec(char a) {
    if (a >= '0' && a <= '9') {
        return a - '0';
    } else if (a >= 'A' && a <= 'F') {
        return 10 + (a - 'A');
    } else if (a >= 'a' && a <= 'f') {
        return 10 + (a - 'a');
    } else {
        return -1;
    }
}

int urldecoder(char *src) {
    int Dec;
    char *dst = src;
    int token;
    while (*src) {
        token = *(src++);
        if (token == '%') {
            Dec = HexToDec(*(src++));
            if (Dec < 0) return 0;
            token = Dec << 4;

            Dec = HexToDec(*(src++));
            if (Dec < 0) return 0;
            token += Dec;
            *(dst++) = token;
        } else if (token == '+') {
            *(dst++) = ' ';
        } else {
            *(dst++) = token;
        }
    }
    *dst++ = 0;
    return 1;
}

int url_queryParse(const char *queryString, const char *parameter, char **output, int *len) {
    char token;
    char *cursor = strstr(queryString, parameter);
    if (cursor == NULL) return 0;
    cursor += strlen(parameter);
    if (*(cursor++) != '=') return 0;
    *output = cursor;
    *len = 0;
    while (1) {
        token = *(cursor++);
        if (token == '&') return 1;
        if (token == 0) return 1;
        *len++;
    }
}

int url_queryParse2(const char *queryString, const char *parameter, char *output, int MaxLen) {
    char token;
    char *cursor = strstr(queryString, parameter);
    if (cursor == NULL) return 0;
    cursor += strlen(parameter);
    if (*(cursor++) != '=') return 0;

    while (MaxLen--) {
        token = *(cursor++);
        if (token == '&') {
            *(output++) = 0;
            return 1;
        } else if (token == 0) {
            *(output++) = 0;
            return 1;
        } else {
            *(output++) = token;
        }
    }
    return 0;
}

void DisposeHTTP_Request(HTTP_REQUEST *req) {
    DISABLE_INTERRUPTS;
    req->rawIdx = req->rawbuffer;
    req->TCP_ChannelID = 0xFF;
    req->Resource = NULL;
    req->Content = NULL;
    req->ContentLength = 0;
    req->ContentRecieved = 0;
    req->ETag = NULL;
    req->Method = HTTP_METHOD_UNKNOWN;
    req->ParseState = HTTP_PARSE_STATE_FREE;
    req->cnonce = NULL;
    req->expires = 0;
    req->nonce = NULL;
    req->nonceCount = NULL;
    req->response = NULL;
    ENABLE_INTERRUPTS;
}

int MoveCursorToLineEnd(char **src, int maxLen) {
    int len = 0;
    char *cursor = *src;
    while (1) {
        if (*cursor == 0 || maxLen == 0) {
            *src = cursor;
            return -2;
        }
        if (*cursor == '\r' || *cursor == '\n') {
            *(cursor++) = 0;
            if (*cursor == '\r' || *cursor == '\n') {
                *(cursor++) = 0;
            }
            *src = cursor;
            return len;
        }
        len++;
        cursor++;
        if (len == maxLen) {
            *src = cursor;
            return -1;
        }
    }
    *src = cursor;
    return 0;
}

void ParseAuthorization(char *line, HTTP_REQUEST *req) {
    req->nonce = NULL;
    req->cnonce = NULL;
    req->response = NULL;
    req->nonceCount = NULL;

    char *nonce = strstr(line, "nonce=\"");
    char *nonceCount = strstr(line, "nc=");
    char *cnonce = strstr(line, "cnonce=\"");
    char *response = strstr(line, "response=\"");
    if (nonce == NULL || nonceCount == NULL || cnonce == NULL || response == NULL) return;

    nonce += 7;
    nonceCount += 3;
    cnonce += 8;
    response += 10;

    char *cursor = (char *) req->rawIdx;
    req->nonce = cursor;
    while (*nonce != '\"') {
        *(cursor++) = *(nonce++);
    }
    *(cursor++) = 0;

    req->nonceCount = cursor;
    while (*nonceCount != ',') {
        *(cursor++) = *(nonceCount++);
    }
    *(cursor++) = 0;

    req->cnonce = cursor;
    while (*cnonce != '\"') {
        *(cursor++) = *(cnonce++);
    }
    *(cursor++) = 0;

    req->response = cursor;
    while (*response != '\"') {
        *(cursor++) = *(response++);
    }
    *(cursor++) = 0;
    req->rawIdx = (BYTE *) cursor;
}

char *myStringCopy(char *src, char *dst) {
    while (*src) {
        *(dst++) = *(src++);
    }
    *(dst++) = 0;
    return dst;
}

char *myStringCopyN(char *src, char *dst, int bCount) {
    while (bCount--) {
        *(dst++) = *(src++);
    }
    *(dst++) = 0;
    return dst;
}

void ParseMessage_HTTP(ESP8266_SLIP_MESSAGE *msg) {
    HTTP_REQUEST *req = NULL;
    int ChannelId = msg->TCP_ChannelID;
    unsigned long tmr;
    GetTime(tmr);
    if (msg->DataLength == 0) return;

    //Is there an existing HTTP Request Aleady?
    int x;
    for (x = 0; x < HTTP_CHANNEL_COUNT; x++) {
        if (HTTP_Channels[x].TCP_ChannelID == ChannelId &&
                HTTP_Channels[x].ParseState != HTTP_PARSE_STATE_FREE) {
            req = &HTTP_Channels[x];
            break;
        }
    }

    if (req == NULL) {
        //Can't find an existing request so let's try and find an empty slot for it
        for (x = 0; x < HTTP_CHANNEL_COUNT; x++) {
            if (HTTP_Channels[x].ParseState == HTTP_PARSE_STATE_FREE || tmr > HTTP_Channels[x].expires) {
                req = &HTTP_Channels[x];
                DisposeHTTP_Request(req);
                req->TCP_ChannelID = ChannelId;
                req->ParseState = HTTP_PARSE_STATE_START;
                req->expires = tmr + (REQUEST_EXPIRATION_TIME * SYSTEM_TIMER_FREQ);
                break;
            }
        }
    }

    //Nothing Existing and can't find a place for it...DROP!
    if (req == NULL) {
        return;
    }

    char *MsgCursor = (char *) msg->Data;
    char *MsgCursorEnd = (char *) msg->Data + msg->DataLength;
    char *LineStart;
    int LineLength;
    while (MsgCursor < MsgCursorEnd) {
        switch (req->ParseState) {
            case HTTP_PARSE_STATE_START: //We're looking for the first HTTP Header line
                LineStart = MsgCursor;
                LineLength = MoveCursorToLineEnd(&MsgCursor, ESP8266_SLIP_MESSAGE_MAX_LEN);
                if (LineLength < 0) {
                    goto InvalidRequest;
                }

                char *LineCursor = LineStart + LineLength; //Start at the end
                char *HTTP_Version = NULL, *Resource = NULL;
                char Verb[4];

                while (LineCursor >= LineStart) { //While we haven't gone beyond the line start...
                    if (*LineCursor == ' ') {
                        *LineCursor = 0;
                        HTTP_Version = LineCursor + 1;
                        break;
                    } else {
                        LineCursor--;
                    }
                }
                if (HTTP_Version == NULL) {
                    Log("Invalid HTTP Request: HTTP_Version==NULL\r\n");
                    goto InvalidRequest;
                }

                if (memcmp(HTTP_Version, "HTTP", 4) != 0) {
                    Log("Invalid HTTP Request: HTTP_Version!=HTTP\r\n");
                    goto InvalidRequest;
                }

                //Now find the space before the Resource
                while (LineCursor >= LineStart) {
                    if (*LineCursor == ' ') {
                        *LineCursor = 0;
                        Resource = LineCursor + 1;
                        break;
                    } else {
                        LineCursor--;
                    }
                }

                if (Resource == NULL) {
                    Log("Invalid HTTP Request: Resource==NULL\r\n");
                    //
                    goto InvalidRequest;
                }

                if (urldecoder(Resource) != 1) {
                    Log("Invalid HTTP Request: Error Decoding URL\r\n");
                    //
                    goto InvalidRequest;
                }

                if (strlen(Resource) > 1536) {
                    Log("Invalid HTTP Request: Resource Too Long\r\n");
                    goto InvalidRequest;
                }

                LineCursor--;
                Verb[3] = 0;
                Verb[2] = *(LineCursor--);
                Verb[1] = *(LineCursor--);
                Verb[0] = *(LineCursor);
                if (LineCursor < LineStart) {
                    Log("Invalid HTTP Request: Short Method\r\n");
                    //
                    goto InvalidRequest;

                }

                if (memcmp(Verb, "GET", 3) == 0) {
                    req->Method = HTTP_METHOD_GET;
                } else if (memcmp(Verb, "PUT", 3) == 0) {
                    req->Method = HTTP_METHOD_PUT;
                } else if (memcmp(Verb, "EAD", 3) == 0) {
                    req->Method = HTTP_METHOD_HEAD;
                } else if (memcmp(Verb, "OST", 3) == 0) {
                    req->Method = HTTP_METHOD_POST;
                } else if (memcmp(Verb, "ETE", 3) == 0) {
                    req->Method = HTTP_METHOD_DELETE;
                } else if (memcmp(Verb, "ACE", 3) == 0) {
                    req->Method = HTTP_METHOD_TRACE;
                } else if (memcmp(Verb, "ECT", 3) == 0) {
                    req->Method = HTTP_METHOD_CONNECT;
                } else if (memcmp(Verb, "ONS", 3) == 0) {
                    req->Method = HTTP_METHOD_OPTIONS;
                } else {
                    Log("Invalid HTTP Request: unknown Method - %s\r\n", Verb);
                    //
                    goto InvalidRequest;
                }

                //                char *caseCursor = Resource;
                //                char inhibitCase = 0;
                //                while (*caseCursor) {
                //                    if (*caseCursor == '=') {
                //                        inhibitCase = 1;
                //                    } else if (*caseCursor == '&') {
                //                        inhibitCase = 0;
                //                    } else if (inhibitCase == 0 && *caseCursor >= 65 && *caseCursor <= 90) {
                //                        *caseCursor += 32;
                //                    }
                //                    caseCursor++;
                //                }

                req->Resource = (char *) req->rawIdx;
                req->rawIdx = (BYTE *) myStringCopy(Resource, req->Resource);
                req->ETag = NULL;
                req->ContentLength = 0;
                req->ParseState = HTTP_PARSE_STATE_HEADERS;
                break;
            case HTTP_PARSE_STATE_HEADERS:
                //We're looking for the Headers Now
                LineStart = MsgCursor;
                LineLength = MoveCursorToLineEnd(&MsgCursor, 512);

                //Is this a blank line... if so that is the end of the header
                if (LineLength <= 0) {
                    if (req->ContentLength == 0) {
                        req->ParseState = HTTP_PARSE_STATE_COMPLETE;
                        return;
                    } else {
                        MsgCursor++;
                        req->ContentRecieved = 0;
                        req->Content = req->rawIdx;
                        req->rawIdx += req->ContentLength;
                        req->ParseState = HTTP_PARSE_STATE_CONTENT;
                    }
                } else {
                    if (strncmp(LineStart, "Content-Length:", 15) == 0) {
                        sscanf(LineStart, "%*s %d", &req->ContentLength);
                    } else if (strncmp(LineStart, "If-None-Match:", 14) == 0) {
                        char *ptrEtagStart = LineStart + 16;
                        int etag_len = strlen(LineStart) - 17;
                        req->ETag = (char *) req->rawIdx;
                        req->rawIdx = (BYTE *) myStringCopyN(ptrEtagStart, req->ETag, etag_len);
                    } else if (strncmp(LineStart, "Authorization:", 14) == 0) {
                        char *ptrDigestStart = LineStart + 14;
                        ParseAuthorization(ptrDigestStart, req);
                    }
                }
                break;
            case HTTP_PARSE_STATE_CONTENT:
                while (MsgCursor <= MsgCursorEnd) {
                    req->Content[req->ContentRecieved] = *(BYTE *) MsgCursor;
                    MsgCursor++;
                    req->ContentRecieved++;
                    if (req->ContentRecieved == req->ContentLength) {
                        req->Content[req->ContentRecieved] = 0;
                        req->ParseState = HTTP_PARSE_STATE_COMPLETE;
                        return;
                    }
                }
                return;
                break;
            default:
                req->ParseState = HTTP_PARSE_STATE_FREE;
                goto InvalidRequest;
                break;
        }
    }
    return;


InvalidRequest:
    //Something went 100% wrong so not only do we want to dispose of the
    //message BUT ALSO the Request...
    DisposeHTTP_Request(req);
    return;
}

void Send404_NotFound(HTTP_REQUEST * req) {
    Log("   %d: Send404_NotFound...", req->TCP_ChannelID);
    const char *HTTP_404 = "HTTP/1.1 404 Not Found\r\n"
            "Connection: close\r\n"
            "Content-Length: 0\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "\r\n";

    int retry = 3;
    while (retry--) {
        ESP_TCP_StartStream(req->TCP_ChannelID);
        circularPrintf(txFIFO, "%s", HTTP_404);
        if (ESP_TCP_TriggerWiFi_Send(req->TCP_ChannelID) == 1) {
            if (ESP_TCP_Wait_WiFi_SendCompleted(req->TCP_ChannelID)) {
                Log("OK\r\n");
                return;
            } else {
                Log("#");
            }
        } else {
            Log("*");
        }
    }
    Log("Failed\r\n");

}

void Send405_MethodNotAllowed(HTTP_REQUEST * req, const char *AllowedMethods) {
    Log("   %d: Send405_MethodNotAllowed...", req->TCP_ChannelID);

    int retry = 3;
    while (retry--) {
        ESP_TCP_StartStream(req->TCP_ChannelID);
        circularPrintf(txFIFO, "HTTP/1.1 405 Method Not Allowed\r\n"
                "Connection: close\r\n"
                "Content-Length: 0\r\n"
                "Allow: %s\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "\r\n", AllowedMethods);
        if (ESP_TCP_TriggerWiFi_Send(req->TCP_ChannelID) == 1) {
            if (ESP_TCP_Wait_WiFi_SendCompleted(req->TCP_ChannelID)) {
                Log("OK\r\n");
                return;
            } else {
                Log("#");
            }
        } else {
            Log("*");
        }
    }
    Log("Failed\r\n");


}

void Send500_InternalServerError(HTTP_REQUEST * req, const char *Msg) {

    Log("   %d: Send500_InternalServerError...'%s'", req->TCP_ChannelID, Msg);

    int retry = 3;
    while (retry--) {
        ESP_TCP_StartStream(req->TCP_ChannelID);
        circularPrintf(txFIFO, "HTTP/1.1 500 Internal Server Error\r\n"
                "Connection: close\r\n"
                "Content-Length: %d\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "\r\n"
                "%s", strlen(Msg), Msg);
        if (ESP_TCP_TriggerWiFi_Send(req->TCP_ChannelID) == 1) {
            if (ESP_TCP_Wait_WiFi_SendCompleted(req->TCP_ChannelID)) {
                Log("OK\r\n");
                return;
            } else {
                Log("#");
            }
        } else {
            Log("*");
        }
    }
    Log("Failed\r\n");
}

void Send501_NotImplemented(HTTP_REQUEST * req) {
    Log("   %d: Send501_NotImplemented...", req->TCP_ChannelID);

    int retry = 3;
    while (retry--) {
        ESP_TCP_StartStream(req->TCP_ChannelID);
        circularPrintf(txFIFO, "HTTP/1.1 501 Not Implemented\r\n"
                "Connection: close\r\n"
                "Content-Length: 0\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "\r\n");
        if (ESP_TCP_TriggerWiFi_Send(req->TCP_ChannelID) == 1) {
            if (ESP_TCP_Wait_WiFi_SendCompleted(req->TCP_ChannelID)) {
                Log("OK\r\n");
                return;
            } else {
                Log("#");
            }
        } else {
            Log("*");
        }
    }
    Log("Failed\r\n");
}

void Send200_OK_Simple(HTTP_REQUEST * req) {
    int channelID = req->TCP_ChannelID;
    //Log("   %d: Send200_OK_Simple...", req->TCP_ChannelID);
    int retry = 3;
    while (retry--) {
        ESP_TCP_StartStream(req->TCP_ChannelID);
        circularPrintf(txFIFO, "HTTP/1.1 200 OK\r\n"
                "Connection: close\r\n"
                "Content-Length: 0\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "\r\n");
        if (ESP_TCP_TriggerWiFi_Send(channelID) == 1) {
            if (ESP_TCP_Wait_WiFi_SendCompleted(channelID)) {
                //Log("OK\r\n");
                return;
            } else {
                Log("#");
            }
        } else {
            Log("*");
        }
    }
    Log("Failed\r\n");
}

void Send200_OK_Data(HTTP_REQUEST * req, unsigned char *msg, int length) {
    Log("   %d: Send200_OK_Data...", req->TCP_ChannelID);
    int retry = 3;
    while (retry--) {
        ESP_TCP_StartStream(req->TCP_ChannelID);
        circularPrintf(txFIFO, "HTTP/1.1 200 OK\r\n"
                "Connection: close\r\n"
                "Content-Type: application/octet-stream\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Content-Length: %i\r\n"
                "\r\n", length);
        ESP_StreamArray(msg, length);
        if (ESP_TCP_TriggerWiFi_Send(req->TCP_ChannelID) == 1) {
            if (ESP_TCP_Wait_WiFi_SendCompleted(req->TCP_ChannelID)) {
                Log("OK\r\n");
                return;
            } else {
                Log("#");
            }
        } else {
            Log("*");
        }
    }
    Log("Failed\r\n");
}

void Send200_OK_SmallMsg(HTTP_REQUEST * req, const char *msg) {
    Log("   %d: Send200_OK_SmallMsg...", req->TCP_ChannelID);
    int retry = 3;
    while (retry--) {
        ESP_TCP_StartStream(req->TCP_ChannelID);
        circularPrintf(txFIFO, "HTTP/1.1 200 OK\r\n"
                "Connection: close\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Content-Length: %i\r\n"
                "\r\n"
                "%s", strlen(msg), msg);
        if (ESP_TCP_TriggerWiFi_Send(req->TCP_ChannelID) == 1) {
            if (ESP_TCP_Wait_WiFi_SendCompleted(req->TCP_ChannelID)) {
                Log("OK\r\n");
                return;
            } else {
                Log("#");
            }
        } else {
            Log("*");
        }
    }
    Log("Failed\r\n");
}

void Send304_NotModified(HTTP_REQUEST * req) {
    Log("   %d: Send304_NotModified...", req->TCP_ChannelID);
    int retry = 3;
    while (retry--) {
        ESP_TCP_StartStream(req->TCP_ChannelID);
        circularPrintf(txFIFO, "HTTP/1.1 304 Not Modified\r\n"
                "Cache-Control: max-age=600\r\n"
                "ETag: \"%s\"\r\n"
                "Connection: close\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "\r\n", req->ETag);
        if (ESP_TCP_TriggerWiFi_Send(req->TCP_ChannelID) == 1) {
            if (ESP_TCP_Wait_WiFi_SendCompleted(req->TCP_ChannelID)) {
                Log("OK\r\n");
                return;
            } else {
                Log("#");
            }
        } else {
            Log("*");
        }
    }
    Log("Failed\r\n");
}

void Send401_Unathorized(HTTP_REQUEST * req, const char *nonce, const char *Stale) {
    Log("   %d: Send401_Unathorized...", req->TCP_ChannelID);
    int retry = 3;
    while (retry--) {
        ESP_TCP_StartStream(req->TCP_ChannelID);
        circularPrintf(txFIFO, "HTTP/1.1 401 Unauthorized\r\n"
                "WWW-Authenticate: Digest realm=\"%s\", qop=\"auth\", nonce=\"%s\", stale=%s\r\n"
                "Content-Type: text/html\r\n"
                "Content-Length: 100\r\n"
                "Connection: close\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "\r\n"
                "<!DOCTYPE html><html><head><title>Error</title></head><body><h1>401 Unauthorized.</h1></body></html>",
                ESP_Config.Name, nonce, Stale);
        if (ESP_TCP_TriggerWiFi_Send(req->TCP_ChannelID) == 1) {
            if (ESP_TCP_Wait_WiFi_SendCompleted(req->TCP_ChannelID)) {
                Log("OK\r\n");
                return;
            } else {
                Log("#");
            }
        } else {
            Log("*");
        }
    }
    Log("Failed\r\n");
}

void RequestAuthorization(HTTP_REQUEST *req, char Stale) {
    static int NextSecurityCTX = 0;
    DIGEST_CTX *ctx = NULL;
    unsigned long tmr;
    GetTime(tmr);
    int x;
    unsigned char rnd;
    for (x = 0; x < DIGEST_CONNECTIONS_MAX; x++) {
        if (Digest_CTXs[NextSecurityCTX].isValid == 0 || tmr > Digest_CTXs[NextSecurityCTX].expires) {
            ctx = &Digest_CTXs[NextSecurityCTX];
            NextSecurityCTX++;
            if (NextSecurityCTX > DIGEST_CONNECTIONS_MAX) NextSecurityCTX = 0;

            ctx->isValid = 1;
            ctx->expires = tmr + NONCE_LIFETIME;
            char *cursor = ctx->nonce;
            for (x = 0; x < 16; x++) {
                while (1) {
                    DISABLE_INTERRUPTS;
                    if (TRNG_fifo->Read != TRNG_fifo->Write) {
                        ENABLE_INTERRUPTS;
                        break;
                    }
                    ENABLE_INTERRUPTS;
                    DELAY_5uS;
                };
                FIFO_Read(TRNG_fifo, rnd);
                cursor += sprintf(cursor, "%02x", rnd ^ (TMR4 & 0xFF));
            }
            *cursor = 0;
            if (Stale) {
                Send401_Unathorized(req, ctx->nonce, "TRUE");
            } else {
                Send401_Unathorized(req, ctx->nonce, "FALSE");
            }
            return;
        }
        NextSecurityCTX++;
        if (NextSecurityCTX > DIGEST_CONNECTIONS_MAX) NextSecurityCTX = 0;
    }
    Send500_InternalServerError(req, "No Free Security Contexts Are Available. Try again later...");
}

const char *HTTP_METHOD_TO_STRING(int method) {
    switch (method) {

        case HTTP_METHOD_GET:
            return "GET";
            break;
        case HTTP_METHOD_PUT:
            return "PUT";
            break;
        case HTTP_METHOD_DELETE:
            return "DELETE";
            break;
    }
    return "";
}

int AuthorizeConnection(HTTP_REQUEST *req) {
    //Log("Digiest Authorization Start: ReqNonce=%s\r\n", req->nonce);
    if (req->nonce == NULL || req->cnonce == NULL ||
            req->nonceCount == NULL || req->response == NULL) {
        RequestAuthorization(req, 0);
        return 0;
    }

    int x;
    unsigned long tmr;
    DIGEST_CTX * ctx = NULL;
    GetTime(tmr);
    for (x = 0; x < DIGEST_CONNECTIONS_MAX; x++) {
        if (strcmp(Digest_CTXs[x].nonce, req->nonce) == 0) {
            if (Digest_CTXs[x].isValid && Digest_CTXs[x].expires > tmr) {
                ctx = &Digest_CTXs[x];
            } else {
                Log("AuthorizeConnection   Failed: Stale\r\n");
                RequestAuthorization(req, 1);
                return 0;
            }
        }
    }

    if (ctx == NULL) {
        Log("AuthorizeConnection   Failed: Unable to find CTX\r\n");
        RequestAuthorization(req, 0);
        return 0;
    }

    MD5_CTX md5_ctx;
    char txtResponse[512];
    char calc_response[34];
    char HA2[34];
    sprintf(txtResponse, "%s:%s", HTTP_METHOD_TO_STRING(req->Method), req->Resource);

    MD5_Init(&md5_ctx);
    MD5_Update(&md5_ctx, txtResponse, strlen(txtResponse));
    MD5_Final(HA2, &md5_ctx);
    //Log("rawHA2=%s  md5HA2=%s\r\n", txtResponse, HA2);
    //sprintf(txtResponse, "%s:%s:%s:%s:auth:%s", SecurityContext.HA1, ctx->nonce, req->nonceCount, req->cnonce, HA2);
    sprintf(txtResponse, "%s:%s:%s:%s:auth:%s", ESP_Config.HA1, ctx->nonce, req->nonceCount, req->cnonce, HA2);
    //Log("rawResponse=%s\r\n", txtResponse);
    MD5_Init(&md5_ctx);
    MD5_Update(&md5_ctx, txtResponse, strlen(txtResponse));
    MD5_Final(calc_response, &md5_ctx);
    //Log("md5Response=%s\r\n", calc_response);
    //Log("request Response=%s\r\n", req->response);
    ctx->isValid = 0;
    ctx->expires = 1;
    if (strcmp(calc_response, req->response) == 0) {
        //Log("match!\r\n");
        return 1;
    }
    //Log("FAIL!\r\n");
    RequestAuthorization(req, 0);
    return 0;
}

char MakeUppercase(char inp) {
    if (inp >= 97 && inp <= 122) return inp - 32;
    return inp;
}

char MakeLowercase(char inp) {
    if (inp >= 65 && inp <= 90) return inp + 32;
    return inp;
}

const char *GetContentType(const char *filename) {
    int fnameLength = strlen(filename) - 3;
    int x;
    char Extension[4];
    for (x = 0; x < 3; x++) {
        Extension[x] = MakeUppercase(filename[fnameLength + x]);
    }
    Extension[3] = 0;

    if (strcmp(Extension, "HTM") == 0) {
        return "text/html";
    } else if (strcmp(Extension, "JPG") == 0) {
        return "image/jpeg";
    } else if (strcmp(Extension, "GIF") == 0) {
        return "image/gif";
    } else if (strcmp(Extension, "TIF") == 0) {
        return "image/tiff";
    } else if (strcmp(Extension, "CSS") == 0) {
        return "text/css";
    } else if (strcmp(Extension, ".JS") == 0) {
        return "text/javascript";
    } else if (strcmp(Extension, "TXT") == 0) {
        return "text/plain";
    } else if (strcmp(Extension, "PNG") == 0) {
        return "image/png";
    } else if (strcmp(Extension, "TML") == 0) {
        return "text/html";
    } else {
        return "application/octet-stream";
    }
}

void Process_GET_File(HTTP_REQUEST * req, int AllowCache) {
    Process_GET_File_ex(req, 0, 0, AllowCache);
}

void Process_GET_File_ex(HTTP_REQUEST * req, unsigned long start, unsigned long length, int AllowCache) {
    ff_File Handle;
    ff_File *handle = &Handle;
    char FileETag[32];
    int retry;

    if (req->Resource == NULL) {
        Send500_InternalServerError(req, "Resource is NULL");
        return;
    }

    BYTE buff[256];
    int res;
    unsigned long bytesSent;
    unsigned long bytesToRead;
    char *resource = req->Resource;
    resource++;

    Log("%d: GET File Resource=\"%s\"\r\n", req->TCP_ChannelID, resource);

    const char *ContentEncoding;
    sprintf((char *) buff, "%s.gz", resource);
    res = ff_OpenByFileName(handle, (char *) buff, 0);
    if (res == FR_OK) {
        ContentEncoding = "gzip";
    } else {
        res = ff_OpenByFileName(handle, resource, 0);
        if (res != FR_OK) {
            Send404_NotFound(req);
            return;
        }
        ContentEncoding = "identity";
    }

    if (req->ETag == NULL) {
        Log("   Request Has No ETAG\r\n");
    } else {
        sprintf(FileETag, "%04x%06lx", handle->UID, handle->Length);
        Log("   File E-Tag=%s Request E-Tag=%s\r\n", FileETag, req->ETag);
        if (memcmp(FileETag, req->ETag, 4) == 0) {
            Send304_NotModified(req);
            return;
        }
    }

    const char *cachePolicy;
    if (AllowCache) {
        cachePolicy = "max-age=604800"; //7 Days
    } else {
        cachePolicy = "no-cache";
        FileETag[0] = 0;
    }

    Log("   Content-Encoding: %s\r\n", ContentEncoding);

    res = ff_Seek(handle, start);
    if (res != FR_OK) {
        sprintf((char *) buff, "Error Seeking File %s RES=%s\r\n", buff, Translate_DRESULT(res));
        Log("   %s\r\n", buff);
        Send500_InternalServerError(req, (char *) buff);
        return;
    }

    if (length == 0) {
        length = handle->Length;
    }
    Log("   Content-Length=%ul\r\n", length);

    ESP_TCP_StartStream(req->TCP_ChannelID);
    circularPrintf(txFIFO, "HTTP/1.1 200 OK\r\n"
            "Connection: close\r\n"
            "Cache-Control: %s\r\n"
            "ETag: \"%s\"\r\n"
            "Content-Type: %s\r\n"
            "Content-Encoding: %s\r\n"
            "Content-Length: %ul\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "\r\n", cachePolicy, FileETag, GetContentType(resource), ContentEncoding, length);

    _U1TXIF = 1;

    Log("      Streaming 1st Chunk:");

    if (length < 11000) bytesToRead = length;
    else bytesToRead = 11000;
    ff_Read_StreamToWifi(handle, bytesToRead);
    Log(" Size=%ul...Trigger Sending...\r\n", bytesToRead);


    if (ESP_TCP_TriggerWiFi_Send(req->TCP_ChannelID) < 0) {
        retry = 3;
        while (retry--) {
            ff_Seek(handle, handle->Position - bytesToRead);
            ESP_TCP_StartStream(req->TCP_ChannelID);
            circularPrintf(txFIFO, "HTTP/1.1 200 OK\r\n"
                    "Connection: close\r\n"
                    "Cache-Control: %s\r\n"
                    "ETag: \"%s\"\r\n"
                    "Content-Type: %s\r\n"
                    "Content-Encoding: %s\r\n"
                    "Content-Length: %ul\r\n"
                    "\r\n", cachePolicy, FileETag, GetContentType(resource), ContentEncoding, length);
            ff_Read_StreamToWifi(handle, bytesToRead);
            if (ESP_TCP_TriggerWiFi_Send(req->TCP_ChannelID) < 0) {
                continue;
            } else {
                break;
            }
        }
        if (retry == 0) {
            Log("Unable to Send Wifi Data...\r\n");
            return;
        }
    }
    bytesSent = bytesToRead;

    while (bytesSent < length) {
        Log("      Streaming NEXT Chunk:");
        ESP_TCP_StartStream(req->TCP_ChannelID);
        if ((length - bytesSent) < 11680) bytesToRead = (length - bytesSent);
        else bytesToRead = 11680;
        ff_Read_StreamToWifi(handle, bytesToRead);
        Log(" Size=%ul...", bytesToRead);

        //The data has been streamed to the ESP.  Now we need to wait for the previous packet to
        //finish sending over WiFi.
        Log("Wait Prev TX...");
        if (!ESP_TCP_Wait_WiFi_SendCompleted(req->TCP_ChannelID)) {
            //There was an error...
            Log("TCP Send Error\r\n");
            return;
        }
        Log("OK...Trigger Send\r\n");
        if (ESP_TCP_TriggerWiFi_Send(req->TCP_ChannelID) < 0) {
            retry = 3;
            while (retry--) {
                ff_Seek(handle, handle->Position - bytesToRead);
                ESP_TCP_StartStream(req->TCP_ChannelID);
                ff_Read_StreamToWifi(handle, bytesToRead);
                if (ESP_TCP_TriggerWiFi_Send(req->TCP_ChannelID) < 0) {
                    continue;
                } else {
                    break;
                }
            }
            if (retry == 0) {
                Log("Unable to Send Wifi Data...\r\n");
                return;
            }
        }
        bytesSent += bytesToRead;
    }
    //The data has been streamed to the ESP.  Now we need to wait for the previous packet to
    //finish sending over WiFi.
    Log("      Wait LAST TX...");
    if (!ESP_TCP_Wait_WiFi_SendCompleted(req->TCP_ChannelID)) {
        //There was an error...
        Log("TCP Send Error %s\r\n", buff);
        return;
    }
    Log("OK\r\n");
    Log("   END GET File\r\n\r\n");
}

void Process_GET_File_blob(HTTP_REQUEST * req) {
    char FileETag[32];
    Log("%d: GET File Resource=\"%s\"\r\n", req->TCP_ChannelID, req->Resource);

    BLOB_FILE handleBacking;
    BLOB_FILE *handle = &handleBacking;
    int res;

    char *cursor = req->Resource;
    while (*cursor) {
        if (*cursor == '?') {
            *cursor = 0;
            break;
        }
        cursor++;
    }

    res = BLOB_openFile(handle, req->Resource);
    if (res != FR_OK) {
        Log("   File Not Found\r\n");
        Send404_NotFound(req);
        return;
    }



    unsigned long fileOffset = 0;
    unsigned long bytesRemaining = handle->length;
    unsigned long bytesLastRead, nextBytesToRead;
    int retry;

    if (req->ETag == NULL) {
        Log("   Request Has No ETAG\r\n");
    } else {
        sprintf(FileETag, "X%04X", handle->checksum);
        Log("   File E-Tag=%s Request E-Tag=%s\r\n", FileETag, req->ETag);
        if (memcmp(FileETag, req->ETag, 4) == 0) {
            Send304_NotModified(req);
            return;
        }
    }

    //Buffer the first packet...
    bytesLastRead = bytesRemaining;
    if (bytesLastRead > 11680) bytesLastRead = 11680;
    //Log("  First Packet: Buffering %xl bytes\r\n", bytesLastRead);
    ESP_TCP_StartStream(req->TCP_ChannelID);
    BLOB_readStreamToWifi(handle, fileOffset, bytesLastRead);
    Log("   Buffered: Offset=%ul Length=%ul\r\n", fileOffset, bytesLastRead);
    _U1TXIF = 1;
    retry = 3;

    while (bytesRemaining) {
        //Send the buffered packet
        while (1) {
            if (ESP_TCP_TriggerWiFi_Send(req->TCP_ChannelID) >= 0) break;
            Log("   Trigger Send Failed..Retry\r\n");
            //The buffered packet failed...so retry to send it again.
            if ((retry--) == 0) goto FailErr;
            ESP_TCP_StartStream(req->TCP_ChannelID);
            BLOB_readStreamToWifi(handle, fileOffset, bytesLastRead);
            Log("   Buffered: Offset=%ul Length=%ul\r\n", fileOffset, bytesLastRead);
            _U1TXIF = 1;
        }
        Log("   Sending...\r\n");
        DELAY_105uS; //Give the esp time to do it's thing...

        //Advance the file forward
        bytesRemaining -= bytesLastRead;
        fileOffset += bytesLastRead;


        //Buffer the next packet
        nextBytesToRead = bytesRemaining;
        if (nextBytesToRead > 11680) nextBytesToRead = 11680;
        if (nextBytesToRead > 0) {
            ESP_TCP_StartStream(req->TCP_ChannelID);
            BLOB_readStreamToWifi(handle, fileOffset, nextBytesToRead);
            Log("   Buffered: Offset=%ul Length=%ul\r\n", fileOffset, nextBytesToRead);
            _U1TXIF = 1;
        }

        //Now wait for the previous packet to finish sending...
        if (!ESP_TCP_Wait_WiFi_SendCompleted(req->TCP_ChannelID)) {
            //There was an error...so let's backup and try to send that packet again...
            bytesRemaining += bytesLastRead;
            fileOffset -= bytesLastRead;
            Log("   !!!WaitSendComplete Failed...Retry bytesRemaining=%l fileOffset=%l\r\n", bytesRemaining, fileOffset);
            ESP_TCP_StartStream(req->TCP_ChannelID);
            BLOB_readStreamToWifi(handle, fileOffset, bytesLastRead);
            Log("   Buffered: Offset=%ul Length=%ul\r\n", fileOffset, bytesLastRead);
            _U1TXIF = 1;
            if ((retry--) == 0) goto FailErr;
        } else {
            Log("   Send Completed OK\r\n");
            //The send was completed so make the latest read the past.
            retry = 3;
            bytesLastRead = nextBytesToRead;
            //Log("Send OK bytesRemaining=%l fileOffset=%l\r\n", bytesRemaining, fileOffset);
        }
    }
    Log("   COMPLETED GET File Resource=\"%s\"\r\n", req->Resource);
    return;
FailErr:
    Log("   FAILED GET File Resource=\"%s\"\r\n", req->Resource);
    return;
}

void HTTP_ServerLoop() {
    char scratchBuffer[1536];
    HTTP_REQUEST *req;
    int x;
    int reqState;

    for (x = 0; x < HTTP_CHANNEL_COUNT; x++) {
        req = &HTTP_Channels[x];
        DISABLE_INTERRUPTS;
        reqState = req->ParseState;
        ENABLE_INTERRUPTS;

        if (reqState == HTTP_PARSE_STATE_COMPLETE) {
            if (req->Resource == NULL) {
                DisposeHTTP_Request(req);
                return;
            }


            char *cc = req->Resource;
            char *dd = scratchBuffer;
            char temp;
            while (1) {
                temp = *(cc++);
                if (temp == '?') break;
                if (temp == 0) break;
                if (temp >= 'a' && temp <= 'z') temp = 'A' + (temp - 'a');
                *(dd++) = temp;
            }
            *(dd++) = 0;
            dd = scratchBuffer;

            if (Global_Config_Mode != 1) {
                if (strstr(dd, "SECURE") != NULL) {
                    Log("Attempted Access to SECURE!\r\n");
                    Send404_NotFound(req);
                }
            }

            if (req->Method == HTTP_METHOD_HEAD ||
                    req->Method == HTTP_METHOD_TRACE ||
                    req->Method == HTTP_METHOD_OPTIONS ||
                    req->Method == HTTP_METHOD_CONNECT) {
                Log("Method Not Implemented: Channel=%d\r\n", req->TCP_ChannelID);
                Send501_NotImplemented(req);
            } else if (memcmp(req->Resource, "/api/", 5) == 0) {
                API_INTERFACE *API = GetAPI(req);
                if (API == NULL) {
                    Send404_NotFound(req);
                } else {
                    if (req->Method == HTTP_METHOD_GET) {
                        if (API->GetAuth) {
                            if (AuthorizeConnection(req)) ProcessAPI(req, API);
                        } else {
                            ProcessAPI(req, API);
                        }
                    } else if (req->Method == HTTP_METHOD_PUT || req->Method == HTTP_METHOD_POST) {
                        if (API->PutAuth) {
                            if (AuthorizeConnection(req)) ProcessAPI(req, API);
                        } else {
                            ProcessAPI(req, API);
                        }
                    } else {
                        Send501_NotImplemented(req);
                    }
                }
            } else if (memcmp(req->Resource, "/device.xml", 11) == 0) {
                uPnP_GetDeviceXML(scratchBuffer);
                Send200_OK_SmallMsg(req, scratchBuffer);
            } else {
                if (req->Method == HTTP_METHOD_GET) {
                    if (req->Resource[0] == '/' && req->Resource[1] == 0) {
                        strcpy(req->Resource, defaultResource);
                    }
                    Process_GET_File_blob(req);
                    //Process_GET_File(req, 1);
                } else {
                    Log("Method Not Allowed: Channel=%d\r\n", req->TCP_ChannelID);
                    Send405_MethodNotAllowed(req, "GET");
                }
            }
            DisposeHTTP_Request(req);
        }
    }
}
