/*
 * notifyrender_test - renders EVERY dom0 notification the gui-agent and notifhost send (every row of
 * notifytexts.h, through the same QerrRenderText the shipped glue uses) and holds each one to the
 * rules of notifyerr.h (rz39):
 *   header   at most 60 characters; no hex code, no file name, no count, no product prefix
 *   body     2 to 4 lines (line 1, an optional cause, the technical line)
 *   tech     the technical line is present and shaped: the sender's executable, its pid, the code
 *            when there is one, the per-boot phrase, "Evidence:"
 *   source   a code is named by its own source's table - every code in this table is a process
 *            exit code and is phrased "exit code N", never as a Windows (SCM) error or a task result
 *   route    the route's redaction accepts the text, and it is under the route's size
 * Prints each rendering verbatim (the AFTER state of these notifications), then the checks.
 *
 * gcc on any host (no Windows), driven by the main repo's tools/tests/notify-render-selftest.sh:
 *   gcc -std=c99 -Wall -Wextra -Werror -I. notifyrender_test.c -o notifyrender_test && ./notifyrender_test
 * Each rule has been SEEN TO FAIL: the runner rebuilds with one defect knob at a time and requires a
 * non-zero exit - NOTIFYTEXT_DEFECT_HEADER, NOTIFYERR_DEFECT_FLATBODY, NOTIFYERR_DEFECT_NOTECH,
 * NOTIFYTEXT_DEFECT_WRONGSOURCE, NOTIFYTEXT_DEFECT_SECRETWORD.
 */
#include "notifytexts.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

static unsigned g_run, g_fail;
static void Check(const char* row, const char* name, int ok)
{
    g_run++;
    if (!ok) g_fail++;
    printf("%s %s: %s\n", ok ? "ok  " : "FAIL", row, name);
}

static int HasCi(const char* hay, const char* needle)
{
    size_t nl = strlen(needle), i, j;
    for (i = 0; hay[i]; i++) {
        for (j = 0; j < nl; j++) {
            char a = hay[i + j], b = needle[j];
            if (!a) return 0;
            if (tolower((unsigned char)a) != tolower((unsigned char)b)) break;
        }
        if (j == nl) return 1;
    }
    return 0;
}

/* a hex code (0x...), a file name (.exe .dll .log .ps1 .txt .sys) or a count (any digit run) */
static int HeaderHasCode(const char* h)     { return strstr(h, "0x") != NULL || strstr(h, "0X") != NULL; }
static int HeaderHasFileName(const char* h)
{
    static const char* const ext[] = { ".exe", ".dll", ".log", ".ps1", ".txt", ".sys", ".cpp", ".c " };
    size_t i;
    for (i = 0; i < sizeof(ext) / sizeof(ext[0]); i++) if (HasCi(h, ext[i])) return 1;
    return 0;
}
static int HeaderHasCount(const char* h)    { for (; *h; h++) if (isdigit((unsigned char)*h)) return 1; return 0; }
static int HeaderHasPrefix(const char* h)   { return HasCi(h, "Qubes Windows Tools") || strchr(h, ':') != NULL; }

/* line i of a CRLF-separated text into out (0 = none) */
static int Line(const char* text, int i, char* out, size_t cap)
{
    const char* p = text;
    int n = 0;
    while (p) {
        const char* e = strstr(p, "\r\n");
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (n == i) {
            if (len >= cap) len = cap - 1;
            memcpy(out, p, len); out[len] = 0;
            return 1;
        }
        n++;
        p = e ? e + 2 : NULL;
    }
    return 0;
}
static int LineCount(const char* text)
{
    int n = 1; const char* p;
    for (p = strstr(text, "\r\n"); p; p = strstr(p + 2, "\r\n")) n++;
    return n;
}

int main(void)
{
    /* the per-instance header name at its longest (the agent caps the image at 32 characters) */
    static const char* const kLongApp = "abcdefghijklmnopqrstuvwxyz012345";
    char text[QERR_MAX_TEXT + 256], line[QERR_MAX_TEXT + 256], tech[QERR_MAX_TEXT + 256];
    size_t i, j;
#if defined(NOTIFYTEXT_DEFECT_HEADER) || defined(NOTIFYERR_DEFECT_FLATBODY) || defined(NOTIFYERR_DEFECT_NOTECH) || \
    defined(NOTIFYTEXT_DEFECT_WRONGSOURCE) || defined(NOTIFYTEXT_DEFECT_SECRETWORD)
    const int defectBuild = 1;
    printf("NOTE: a defect knob is compiled in - this suite MUST fail now.\n");
#else
    const int defectBuild = 0;
#endif

    printf("==== %u notifications (notifytexts.h), rendered as dom0 receives them ====\n", (unsigned)QERR_TEXT_COUNT);
    for (i = 0; i < QERR_TEXT_COUNT; i++) {
        const QerrText* t = &QerrTexts[i];
        const char* arg = strstr(t->header, "%s") ? "chrome" : NULL;
        size_t n = QerrRenderText(text, sizeof(text), t, arg, 4242);
        int lines;
        printf("\n---- %s (%s.%s, %s)%s ----\n%s\n", t->key, t->component, t->id,
               t->sev == QERR_SEV_ACTION ? "ACTION" : t->sev == QERR_SEV_DEGRADED ? "DEGRADED - stays in the log by policy" : "INFO",
               arg ? " [header %s = chrome]" : "", n ? text : "(did not render)");
        Check(t->key, "renders", n > 0);
        if (!n) continue;
        Check(t->key, "component and id pass the route's name rule",
              QerrNameValid(t->component, QERR_MAX_COMPONENT) && QerrNameValid(t->id, QERR_MAX_ID));
        for (j = 0; j < i; j++) if (strcmp(QerrTexts[j].key, t->key) == 0) break;
        Check(t->key, "key is unique", j == i);

        /* header */
        Line(text, 0, line, sizeof(line));
        Check(t->key, "header <= 60 characters", strlen(line) <= 60);
        Check(t->key, "header has no hex code", !HeaderHasCode(line));
        Check(t->key, "header has no file name", !HeaderHasFileName(line));
        Check(t->key, "header has no count", !HeaderHasCount(line));
        Check(t->key, "header has no product prefix", !HeaderHasPrefix(line));
        Check(t->key, "header is a sentence (capital first letter, no trailing period)",
              isupper((unsigned char)line[0]) && line[strlen(line) - 1] != '.');
        if (arg) {
            char hdr[200];
            QerrFormatHeader(hdr, sizeof(hdr), t->header, kLongApp);
            Check(t->key, "header <= 60 characters with the longest app name", strlen(hdr) <= 60);
        }

        /* body */
        lines = LineCount(text);
        Check(t->key, "body is 2 to 4 lines", lines >= 3 && lines <= 5);

        /* the technical line: the last line, shaped by QerrFormatTechLine */
        Line(text, lines - 1, tech, sizeof(tech));
        Check(t->key, "technical line names the sender's executable first", strncmp(tech, t->subject, strlen(t->subject)) == 0);
        Check(t->key, "technical line carries the pid", strstr(tech, " pid 4242") != NULL);
        Check(t->key, "technical line says how often it is reported", strstr(tech, "; reported once per boot") != NULL);
        Check(t->key, "technical line says where the evidence is", strstr(tech, ". Evidence: ") != NULL && strstr(tech, t->evidence) != NULL);
        Check(t->key, "technical line is one line (nothing after it)", strstr(tech, "\n") == NULL);

        /* the cause and the code's source */
        if (t->code) {
            Check(t->key, "the code is phrased by the process table (exit code N), its source", strncmp(t->code, "exit code ", 10) == 0);
            Check(t->key, "the cause line names the code", t->cause && strstr(t->cause, t->code) != NULL);
            Check(t->key, "the technical line names the code", strstr(tech, t->code) != NULL);
            Check(t->key, "the cause is not phrased by another table (Windows error / result)",
                  !HasCi(t->cause, "Windows error") && !HasCi(t->cause, "result 0x"));
        }
        if (t->cause) Check(t->key, "the cause line starts with Cause:", strncmp(t->cause, "Cause: ", 7) == 0);

        /* the route */
        Check(t->key, "the route's redaction accepts the text", QerrRedactReason(text) == NULL);
        Check(t->key, "under the route's size", n <= QERR_MAX_TEXT);
        Check(t->key, "no 'death n this boot' (that phrase is the death reporter's)", strstr(text, " this boot") == NULL);
    }

    printf("--- %u checks, %u failed%s\n", g_run, g_fail, defectBuild ? " (defect build: failure expected)" : "");
    if (defectBuild && g_fail == 0) {
        printf("DEFECT BUILD PASSED - the rule it breaks is decoration. FAIL.\n");
        return 3;
    }
    return g_fail ? 1 : 0;
}
