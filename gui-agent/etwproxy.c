/*
 * etwproxy - launch + exit-wait supervision of `etwproxy.exe`, the least-privilege home
 * of the untrusted ETW/TDH parse (docs/DESIGN-p3-classifier-impl.md sec 10.10/10.14,
 * revised by the 2026-09-05 capability-grant split and the same-day CONSOLE SPLIT: the
 * proxy is its own GUI-DLL-free binary now, not a notifhost.exe mode). See etwproxy.h
 * for why this is a separate file from main.c.
 *
 * WHAT RUNS WHERE (capability-grant two-context split, ZERO new processes). This SYSTEM
 * agent is the ETW session CONTROLLER: per launch it reaps any stale session by name,
 * starts the real-time session `QubesToastBridgeEtw` with a FIXED private GUID in
 * Wnode.Guid (the deterministic grant target), enables the constant provider list, and
 * grants the consumer account TRACELOG_ACCESS_REALTIME | WMIGUID_QUERY (the least-privilege
 * realtime-consume pair - see EtwCtlGrantConsumer) on the session GUID via
 * EventAccessControl. None of these control calls parses one attacker-influenceable byte
 * (fixed session name, fixed GUID, constant provider list, fixed grant SID), so running
 * them at SYSTEM does not violate the never-SYSTEM rule - that rule is about the
 * untrusted-DATA path. `etwproxy.exe` is then a pure CONSUMER: OpenTraceW +
 * ProcessTrace + the TDH property location, authorized solely by the per-session DACL
 * grant, under the dedicated local account `qubes-etwproxy` whose token NEVER - at any
 * instant - held SeSystemProfilePrivilege or the Performance Log Users group SID. This
 * closes design sec 10.17.2 (PLU let a subverted proxy start/consume real-time sessions
 * for ARBITRARY machine-wide providers): group membership cannot be shed in-process
 * (SE_GROUP_MANDATORY), so the only token that cannot use PLU is one that never had it.
 * The agent contributes exactly the pieces that need SYSTEM and nothing else:
 *   * the session control calls above (StartTraceW/EnableTraceEx2/EventAccessControl);
 *   * a primary token for the consumer account (LogonUserW BATCH - SeBatchLogonRight is
 *     granted explicitly by provisioning, since PLU no longer carries it in);
 *   * the sec 10.14.4 sandbox: job object with 64 MB process-memory cap, 1-process
 *     limit, KILL_ON_JOB_CLOSE, all UI restrictions; CREATE_SUSPENDED -> assign job ->
 *     resume, so the proxy never executes one instruction outside the job;
 *   * session 0 placement (the token is born in this service's session; ETW sessions
 *     and the pipe namespace are machine-global, so session 0 is both sufficient and
 *     the most isolated choice); no user profile is loaded (the proxy needs no HKCU);
 *   * NO window station at all (the 2026-09-05 CONSOLE SPLIT, superseding sec 10.20.3's
 *     dedicated-winsta machinery): the old `notifhost --etw-proxy` died 0xC0000142
 *     STATUS_DLL_INIT_FAILED because notifhost.exe statically imports user32+gdi32
 *     (WinRT --bridge code) and their DllMain connects to a window station the bare
 *     batch token in session 0 does not have. etwproxy.exe links NEITHER (nor WinRT) -
 *     it never touches a winsta, so the failure is impossible by construction and this
 *     agent no longer creates, ACLs, or names any window station or desktop for it
 *     (si.lpDesktop stays NULL; nothing in the proxy ever consumes it). Zero rights
 *     were widened anywhere to achieve this - that is why the owner chose this option;
 *   * the bridge user's SID on the command line (--client-sid), which the proxy bakes
 *     into its pipe DACL - connect/read for that ONE principal.
 * The agent never touches the pipe and never parses an event (sec 10.10.1).
 *
 * CREDENTIALS ARE IN-MEMORY, NO SECRET AT REST. The installer creates the account with a
 * random password it immediately discards (the account is unusable until armed); at every
 * launch THIS agent generates a fresh CSPRNG password, sets it with NetUserSetInfo(1003),
 * and immediately proves it with LogonUserW(LOGON32_LOGON_BATCH) - the
 * guest/set-autologon.ps1 discipline (validate the credential + the batch right before
 * use) with the storage step deleted, because there is nothing to store: the password
 * lives only in this function's stack frame and is zeroed before return. This removes the
 * former LSA secret L$QubesEtwProxyCred, the QubesEtwProxyGuard boot rotation task, and
 * the rotation-vs-retrieve TOCTOU race outright - the agent is the single credential
 * actor, so there is no second writer to race. An agent restart simply resets the
 * password again (nothing else consumes it).
 *
 * GRACEFUL DEGRADE (managed/domain/hardened images): if the account does not exist
 * (provisioning skipped cleanly at install time), or batch logon is refused, this
 * module logs ONE line and parks for the boot. Nothing retries, nothing dialogs,
 * nothing touches the agent's frame path: the bridge's ETW tier simply stays down and
 * its IPC client already degrades to the listener/DB rung (fail-open, sec 10.11.4).
 * A proxy that exits 5 (consume access denied - the DACL grant did not suffice for
 * OpenTrace/ProcessTrace on this build, sec 10.16.3b's datum under the split) or 9
 * (the proxy refused its own token: never-SYSTEM guard, or its drift census) also
 * parks: relaunching cannot fix a rights problem, and the treadmill would only spam
 * the log. A DRIFTED consumer token (Performance Log Users SID /
 * SeSystemProfilePrivilege / Administrators - census before launch) parks too, WITHOUT
 * launching: refuse-on-drift is the secure default, since such a token holds
 * machine-wide trace capability that cannot be shed in-process. Parking stops the
 * session too - nothing would consume it.
 *
 * SUPERVISION IS EXIT-WAIT (owner directive; sec 10.14.6): RegisterWaitForSingleObject
 * on the process handle; the callback fires the moment the proxy exits and schedules a
 * relaunch on a one-shot timer-queue timer with exponential backoff 5 s -> 5 min
 * (reset after 10 min of healthy uptime). No heartbeat file, no staleness poll: a
 * hung-but-alive proxy is indistinguishable from a silent provider and costs only the
 * ETW tier, which is not an urgent condition (sec 10.14.6). EtwProxyPoke() is not a
 * health check - it only covers "no console session yet" at first launch and "console
 * user changed" (new pipe-DACL SID needed), conditions no exit-wait can observe.
 * Session lifecycle rides supervision: every launch stops-then-starts the session
 * (stale reap included), every park and the agent shutdown stop it.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <lm.h>          // NetUserSetInfo(1003) - the in-memory password reset
#include <bcrypt.h>      // BCryptGenRandom (CSPRNG password material)
#include <wincrypt.h>    // CryptoAPI SHA-1 (TraceLogging provider name -> GUID hash)
#include <wmistr.h>      // WNODE_HEADER + WMIGUID_* (evntrace.h prerequisite)
#include <evntrace.h>    // StartTraceW/ControlTraceW/EnableTraceEx2 + TRACELOG_*
#include <evntcons.h>    // EventAccessControl / EventSecuritySetDACL|AddDACL
#include <strsafe.h>

#include "etwproxy.h"

// windows-utils
#include <log.h>

#pragma comment(lib, "userenv.lib")    // CreateEnvironmentBlock for the proxy token
#pragma comment(lib, "netapi32.lib")   // NetUserSetInfo
#pragma comment(lib, "bcrypt.lib")     // BCryptGenRandom
// advapi32 (LogonUserW, ConvertSidToStringSidW, Crypt*, the ETW control surface) and
// wtsapi32 are already linked by the project / main.c's pragma.

// Shared contract with guest/provision-etwproxy-account.ps1 - change both or neither.
// The account is created there (NetUserAdd level 1: NO implicit groups; NO Performance
// Log Users membership under the split; SeBatchLogonRight granted explicitly; interactive/
// remote/network logon denied; per-SID outbound firewall block). The password is OURS:
// generated fresh in memory at every launch, never stored (no LSA secret any more).
#define ETWPROXY_ACCOUNT      L"qubes-etwproxy"

// Shared contract with tools/notifhost/qtb_shared.h (kEtwBridgeSession) - change both or
// neither. The name is what the consumer opens (OpenTraceW LoggerName) and what the reap
// stops; the GUID is the session's Wnode.Guid = the object the EventAccessControl DACL
// grant attaches to. FIXED so the grant target is deterministic across launches (an
// auto-generated per-session GUID would orphan the persisted WMI\Security value).
#define ETWPROXY_SESSION_NAME L"QubesToastBridgeEtw"
static const GUID ETWPROXY_SESSION_GUID = /* generated once for this project, registered nowhere else */
{ 0x7c31f9a2, 0x0d5e, 0x4c8b, { 0x9a, 0x41, 0x5d, 0x2e, 0x8f, 0x66, 0x3b, 0xd4 } };

// NO ETWPROXY_WINSTA_*/ETWPROXY_DESK_* here anymore (2026-09-05 CONSOLE SPLIT): the
// dedicated window-station machinery (design sec 10.20.3) existed only because
// notifhost.exe imported user32/gdi32, whose process-init winsta connect failed
// 0xC0000142 under the winsta-less batch token. etwproxy.exe links no GUI DLL, so the
// proxy needs no window station, no desktop, and no winsta ACEs - the whole apparatus
// (private winsta creation, DACL builder, lpDesktop) is REMOVED, not relocated.

#define ETWPROXY_MEM_LIMIT    (64ull * 1024 * 1024)  // sec 10.14.4: a decode bomb dies, the guest does not
#define ETWPROXY_BACKOFF_MIN  5000                    // ms; sec 10.14.6 (5 s -> 5 min)
#define ETWPROXY_BACKOFF_MAX  300000
#define ETWPROXY_HEALTHY_MS   (10 * 60 * 1000)        // uptime that resets the backoff
#define ETWPROXY_POKE_MS      5000                    // Poke self-throttle (launch precondition only)
#define ETWPROXY_EXIT_DENIED    5   // proxy: OpenTrace/consume access denied under the DACL
                                    // grant - sec 10.16.3b's datum as redefined by the split
                                    // ("does the per-session realtime+query grant suffice")
#define ETWPROXY_EXIT_BADTOKEN  9   // proxy: refused its own token - never-SYSTEM/admin
                                    // guard tripped (launch plumbing handed it the wrong
                                    // token) or its census found drift (PLU group /
                                    // SeSystemProfilePrivilege - machine-wide trace
                                    // capability the decode loop must never hold)
#define ETWPROXY_EXIT_CONSUME   7   // proxy: consumer open/thread/event failure
#define ETWPROXY_EXIT_PIPE      8   // proxy: pipe creation failed (squatter) or 5
                                    // consecutive connect failures (transients re-listen)
#define ETWPROXY_EXIT_KILLED    1   // NOT a proxy return path: the exit code TerminateProcess
                                    // IMPOSES on a force-kill (taskkill /f, Stop-Process
                                    // -Force, or our OWN stop-by-name reap of a stale
                                    // instance). Expected on any SUPERVISED kill - park,
                                    // shutdown, job-kill, the next-launch reap, and the p3a
                                    // T5/T8c drills - so it is logged quietly (its own token,
                                    // still greppable) and relaunched, NOT raised as
                                    // EtwProxyUnknownExit. That anomaly stays reserved for a
                                    // TRULY unaccounted code (a crash: 0xC0000005 / __fastfail
                                    // / a CRT abort) - a benign external kill by 1 must not
                                    // desensitize it ([[fallbacks-are-anomalies]]).
// The proxy's COMPLETE self-return exit-code set is {0, 5, 7, 8, 9} (tools/notifhost/
// etwproxy.cpp header - shared contract, change both or neither); 1 is the externally-imposed
// force-kill code above. Any OTHER rc CANNOT come from the binary's own paths NOR from a
// supervised kill: it means an uncommanded crash (which leaves a CRASH breadcrumb in
// etw-proxy.log via BridgeCrashFilter). EtwProxyExitCb logs THOSE as a LOUD anomaly instead
// of a routine exit ([[fallbacks-are-anomalies]]).

typedef enum
{
    EPS_DISABLED = 0,   // gate off - inert for the whole run
    EPS_IDLE,           // armed, not running: launch as soon as a console user exists
    EPS_RUNNING,        // proxy alive, exit-wait registered
    EPS_BACKOFF,        // exited; one-shot relaunch timer pending
    EPS_PARKED,         // permanent for this boot (account absent / logon denied / token
                        // drift / rc 5 / rc 9)
} ETWPROXY_STATE;

static BOOL             g_Inited;
static BOOL             g_Enabled;
static BOOL             g_Shutdown;
static CRITICAL_SECTION g_Lock;         // guards everything below
static ETWPROXY_STATE   g_State = EPS_DISABLED;
static HANDLE           g_Job;
static HANDLE           g_Proc;
static HANDLE           g_Wait;         // RegisterWaitForSingleObject handle
static HANDLE           g_Timer;        // one-shot relaunch timer (timer queue)
static ULONGLONG        g_LaunchTick;
static DWORD            g_Backoff = ETWPROXY_BACKOFF_MIN;
static ULONGLONG        g_NextPoke;     // Poke throttle (main-loop thread only)
static WCHAR            g_ClientSid[192]; // SID string the running proxy was launched with
static BOOL             g_SessLive;     // this agent started the ETW session and owns stopping it
static BOOL             g_CensusLogged; // token group/priv census printed once per boot

static VOID CALLBACK EtwProxyExitCb(PVOID context, BOOLEAN timedOut);
static VOID CALLBACK EtwProxyRelaunchCb(PVOID context, BOOLEAN timerFired);
static void EtwProxyTryLaunchLocked(void);

// ---- ETW session controller (agent-side; the sec 10.14 control calls) ----------------
// The EtwSessionStop-first reap pattern, moved here from notifhost.cpp now that the agent
// owns the session. A real-time session is KERNEL state that outlives a crashed process;
// stopping by name unconditionally (not-found is the normal case) prevents an eternal
// ERROR_ALREADY_EXISTS.

typedef struct
{
    EVENT_TRACE_PROPERTIES Props;
    WCHAR Name[64];   // ETWPROXY_SESSION_NAME fits with slack
} ETWPROXY_TRACE_PROPS;

static void EtwCtlInitProps(ETWPROXY_TRACE_PROPS* tp)
{
    ZeroMemory(tp, sizeof(*tp));
    tp->Props.Wnode.BufferSize = sizeof(*tp);
    tp->Props.LoggerNameOffset = (ULONG)FIELD_OFFSET(ETWPROXY_TRACE_PROPS, Name);
}

static void EtwCtlSessionStop(void)
{
    ETWPROXY_TRACE_PROPS tp;
    EtwCtlInitProps(&tp);
    ControlTraceW(0, ETWPROXY_SESSION_NAME, &tp.Props, EVENT_TRACE_CONTROL_STOP);
}

// Stop the session if this agent started one. Callers hold g_Lock.
static void EtwProxySessionStopLocked(void)
{
    if (!g_SessLive)
        return;
    EtwCtlSessionStop();
    g_SessLive = FALSE;
}

static ULONG EtwCtlSessionStart(TRACEHANDLE* out)
{
    ETWPROXY_TRACE_PROPS tp;
    EtwCtlInitProps(&tp);
    tp.Props.Wnode.Guid = ETWPROXY_SESSION_GUID;   // FIXED: the deterministic grant target
    tp.Props.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    tp.Props.Wnode.ClientContext = 1;              // QPC; consumer still receives FILETIME
    tp.Props.LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    tp.Props.BufferSize = 64;                      // KB/buffer - notification traffic is tiny
    // L2 FIX (design sec 10.20.4, the measured events=0): a real-time session delivers to
    // ProcessTrace on BUFFER FLUSH, and 64 KB buffers never fill from ~200 B sparse
    // notification events - the proxy's consumer would receive nothing while a file-mode
    // logman capture of the SAME providers shows hundreds (buffers flush to the ETL at
    // stop). FlushTimer=1 forces a per-second flush => <= 1 s delivery. notifhost.cpp's
    // EtwSessionStart (--dump-etw) carries the same fix - change both or neither.
    tp.Props.FlushTimer = 1;
    EtwCtlSessionStop();                           // reap a stale/crashed prior session first
    *out = 0;
    return StartTraceW(out, ETWPROXY_SESSION_NAME, &tp.Props);
}

// Candidate notification providers - shared contract with tools/notifhost/notifhost.cpp
// (g_etwProviders): change both or neither. hashName=TRUE marks a TraceLogging provider
// whose GUID is derived from the name at runtime (the standard EventSource name hash);
// enabling a GUID nothing registers SUCCEEDS (events just never arrive). Extend only from
// --dump-etw evidence, never from blog posts.
typedef struct { const WCHAR* Name; GUID Guid; BOOL HashName; } ETWPROXY_PROVIDER;
static const ETWPROXY_PROVIDER g_EtwProviders[] =
{
    { L"Microsoft-Windows-PushNotifications-Platform",
      { 0x88CD9180, 0x4491, 0x4640, { 0xB5, 0x71, 0xE3, 0xBE, 0xE2, 0x52, 0x79, 0x43 } }, FALSE },
    { L"Microsoft.Windows.Shell.NotificationController", { 0 }, TRUE },
    { L"Microsoft-Windows-Notifications",                { 0 }, TRUE },
    { L"Microsoft.Windows.Notifications.WpnCore",        { 0 }, TRUE },
    { L"Microsoft.Windows.Notifications.WpnApps",        { 0 }, TRUE },
};

// TraceLogging provider name -> GUID (the EventSource hash): SHA-1 over a fixed namespace
// GUID + the UPPERCASED provider name in UTF-16BE; the first 16 digest bytes are read the
// way .NET Guid(byte[]) reads them (Data1..Data3 little-endian) with the high nibble of
// digest byte 7 forced to 5. Port of notifhost.cpp EtwNameToGuid (same contract).
static BOOL EtwCtlNameToGuid(const WCHAR* name, GUID* out)
{
    static const BYTE ns[16] = { 0x48, 0x2C, 0x2D, 0xB2, 0xC3, 0x90, 0x47, 0xC8,
                                 0x87, 0xF8, 0x1A, 0x15, 0xBF, 0xC1, 0x30, 0xFB };
    BYTE data[16 + 256 * 2];
    DWORD len = 16;
    memcpy(data, ns, 16);
    for (const WCHAR* p = name; *p; p++)
    {
        WCHAR c = *p;
        if (c >= L'a' && c <= L'z') c = (WCHAR)(c - 32);   // provider names are ASCII
        if (len + 2 > sizeof(data)) return FALSE;          // constant table: never happens
        data[len++] = (BYTE)(c >> 8);                      // UTF-16 BIG-endian
        data[len++] = (BYTE)(c & 0xFF);
    }
    BYTE dig[20];
    DWORD dl = sizeof(dig);
    HCRYPTPROV cp = 0;
    HCRYPTHASH h = 0;
    BOOL ok = FALSE;
    if (CryptAcquireContextW(&cp, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
    {
        if (CryptCreateHash(cp, CALG_SHA1, 0, 0, &h) &&
            CryptHashData(h, data, len, 0) &&
            CryptGetHashParam(h, HP_HASHVAL, dig, &dl, 0) && dl >= 16)
            ok = TRUE;
        if (h) CryptDestroyHash(h);
        CryptReleaseContext(cp, 0);
    }
    if (!ok)
        return FALSE;
    out->Data1 = (DWORD)dig[0] | ((DWORD)dig[1] << 8) | ((DWORD)dig[2] << 16) | ((DWORD)dig[3] << 24);
    out->Data2 = (USHORT)((USHORT)dig[4] | ((USHORT)dig[5] << 8));
    out->Data3 = (USHORT)((USHORT)dig[6] | ((USHORT)((dig[7] & 0x0F) | 0x50) << 8));
    memcpy(out->Data4, dig + 8, 8);
    return TRUE;
}

// Enable the constant provider list on the session. Every EnableTraceEx2 RC is logged
// verbatim - the "PROXY RC" datum lines of design sec 10.16.4 move agent-side with the
// calls. Returns how many enabled cleanly.
static int EtwCtlEnableProviders(TRACEHANDLE session)
{
    int enabled = 0;
    for (ULONG i = 0; i < (ULONG)RTL_NUMBER_OF(g_EtwProviders); i++)
    {
        GUID g = g_EtwProviders[i].Guid;
        if (g_EtwProviders[i].HashName && !EtwCtlNameToGuid(g_EtwProviders[i].Name, &g))
        {
            LogWarning("PROXY RC api=EnableTraceEx2 provider=%s rc=guid-hash-failed (skipped)",
                       g_EtwProviders[i].Name);
            continue;
        }
        // MatchAnyKeyword ~0 + VERBOSE takes everything each provider offers
        // (TraceLogging events often carry keyword 0, which a narrow mask would drop).
        ULONG rc = EnableTraceEx2(session, &g, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                                  TRACE_LEVEL_VERBOSE, ~0ULL, 0, 0, NULL);
        LogInfo("PROXY RC api=EnableTraceEx2 provider=%s rc=%lu", g_EtwProviders[i].Name, rc);
        if (rc == ERROR_SUCCESS)
            enabled++;
    }
    return enabled;
}

// The capability grant: give the consumer account the least-privilege REALTIME-CONSUME
// pair on the session GUID, and nothing else. CALLER CONTRACT: this MUST run BEFORE
// EtwCtlSessionStart - the logger snapshots its SD from WMI\Security[GUID] at creation, so
// a grant issued after StartTrace never reaches the running session and the consumer's
// realtime connect is denied (the rig-measured exit-5 defect). Two calls, SET-then-ADD:
//   1. SET a DACL containing only the SYSTEM full-control ACE. SET (not a bare ADD)
//      means the registry-persisted WMI\Security value for this GUID is REPLACED on every
//      launch - it can never accrete drift across account re-creations or old grants
//      (the design's stated reason for SET). The SYSTEM ACE must be present because SET
//      replaces the default SD outright, and this agent still needs ControlTrace STOP on
//      the same GUID for reap/shutdown - a consumer-only DACL would lock the controller
//      out of its own session.
//   2. ADD the consumer's allow ACE: TRACELOG_ACCESS_REALTIME | WMIGUID_QUERY.
//      TRACELOG_ACCESS_REALTIME alone was PROVEN INSUFFICIENT on the rig (2026-09-06,
//      Win10 19045: grant rc=0, correctly ordered before StartTrace, OpenTraceW ok,
//      ProcessTrace ERROR_ACCESS_DENIED -> proxy exit 5). Cause: the consumer-side
//      realtime path issues an implicit EVENT_TRACE_CONTROL_QUERY on the session
//      (sechost!EtwpQueryRealTimeTraceProperties, invoked from OpenTrace/ProcessTrace),
//      and a query is gated by WMIGUID_QUERY on the session's SD - "Allows the user to
//      query information about the trace session. Set this permission on the session's
//      GUID" (EventAccessControl docs, evntcons.h). The Performance Log Users route the
//      same docs offer as the alternative works via default-SD ACEs that carry
//      WMIGUID_QUERY alongside TRACELOG_ACCESS_REALTIME - a superset of this pair. The
//      pair is deliberately NOT the PLU set: no TRACELOG_CREATE_REALTIME/ONDISK (could
//      UPDATE/restart the session), no TRACELOG_GUID_ENABLE/REGISTER_GUIDS/LOG_EVENT
//      (provider rights), so the consumer can consume + query THIS session and control
//      nothing.
// Net effect per launch: a deterministic two-ACE DACL {SYSTEM: full, qubes-etwproxy:
// realtime-consume + query}.
static ULONG EtwCtlGrantConsumer(PSID consumerSid)
{
    BYTE sysSid[SECURITY_MAX_SID_SIZE];
    DWORD cb = sizeof(sysSid);
    if (!CreateWellKnownSid(WinLocalSystemSid, NULL, sysSid, &cb))
        return GetLastError();
    GUID g = ETWPROXY_SESSION_GUID;   // EventAccessControl wants a non-const LPGUID
    ULONG rc = EventAccessControl(&g, (ULONG)EventSecuritySetDACL,
                                  (PSID)sysSid, WMIGUID_ALL_ACCESS, TRUE);
    LogInfo("PROXY RC api=EventAccessControl op=SetDACL principal=SYSTEM rc=%lu", rc);
    if (rc != ERROR_SUCCESS)
        return rc;
    rc = EventAccessControl(&g, (ULONG)EventSecurityAddDACL,
                            consumerSid, TRACELOG_ACCESS_REALTIME | WMIGUID_QUERY, TRUE);
    LogInfo("PROXY RC api=EventAccessControl op=AddDACL principal=consumer rights=TRACELOG_ACCESS_REALTIME|WMIGUID_QUERY rc=%lu", rc);
    return rc;
}

// ---- one-shot park: log exactly once, then nothing else this boot (deliverable rule:
// "log once and DO NOTHING ELSE - the ETW tier stays down, the bridge falls to
// listener/DB"). Also stops the agent-owned session: parked means nothing will consume
// it this boot. Callers hold g_Lock.
static void EtwProxyParkLocked(const char* reason, DWORD code)
{
    EtwProxySessionStopLocked();
    g_State = EPS_PARKED;
    LogWarning("ETWPROXYSUP parked for this boot: %S (code %lu) - ETW toast tier stays down, "
               "bridge degrades to listener/DB (fail-open)", reason, code);
}

// ---- backoff: schedule one relaunch attempt; the session is stopped meanwhile (a
// session nobody consumes only accumulates loss counters). Callers hold g_Lock.
static void EtwProxyBackoffLocked(void)
{
    EtwProxySessionStopLocked();
    g_State = EPS_BACKOFF;
    if (!CreateTimerQueueTimer(&g_Timer, NULL, EtwProxyRelaunchCb, NULL, g_Backoff, 0,
                               WT_EXECUTEONLYONCE | WT_EXECUTELONGFUNCTION))
        g_State = EPS_IDLE;   // timer refused: Poke's throttle becomes the retry pace
    g_Backoff = min(g_Backoff * 2, ETWPROXY_BACKOFF_MAX);
}

// ---- credentials: generate + set + prove, all in memory (the in-memory creds contract) -
// The agent is the ONLY credential actor. Per launch: fresh CSPRNG password -> the
// account's password is RESET to it (NetUserSetInfo 1003; SYSTEM needs no old password) ->
// LogonUserW(BATCH) immediately proves both the credential and the batch-logon right
// before anything uses the token (the set-autologon.ps1 validate discipline; here the
// validation logon IS the use, so nothing can diverge between proof and use). The
// password is zeroed before return and stored NOWHERE - no LSA secret, no file, no
// registry, no guard task, no rotation race (design sec 10.17.5 is closed by removal).
static BOOL EtwProxyGenPassword(WCHAR* buf, size_t cch)
{
    BYTE rnd[40];
    if (cch < RTL_NUMBER_OF(rnd) + 5)
        return FALSE;
    if (BCryptGenRandom(NULL, rnd, sizeof(rnd), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        return FALSE;
    // Fixed 4-char prefix guarantees every complexity class a local password policy might
    // demand (upper/lower/digit/symbol) without reducing the 40 random chars (~238 bits).
    buf[0] = L'Q'; buf[1] = L'x'; buf[2] = L'9'; buf[3] = L'!';
    static const WCHAR set[] = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    for (size_t i = 0; i < RTL_NUMBER_OF(rnd); i++)
        buf[4 + i] = set[rnd[i] % 62];
    buf[4 + RTL_NUMBER_OF(rnd)] = 0;
    SecureZeroMemory(rnd, sizeof(rnd));
    return TRUE;
}

// Returns a BATCH primary token for qubes-etwproxy, or NULL with *parkReason set when the
// account is unusable on this image (absent = provisioning skipped on managed/hardened
// images - the graceful-skip case; logon refused = rights swept or provisioning predates
// the no-PLU contract, which grants SeBatchLogonRight explicitly).
static HANDLE EtwProxyLogon(const char** parkReason, DWORD* parkCode)
{
    WCHAR pw[64];
    HANDLE token = NULL;
    *parkReason = NULL;
    *parkCode = 0;

    if (!EtwProxyGenPassword(pw, RTL_NUMBER_OF(pw)))
    {
        *parkReason = "CSPRNG password generation failed";
        *parkCode = GetLastError();
        return NULL;
    }

    USER_INFO_1003 ui;
    ui.usri1003_password = pw;
    NET_API_STATUS ns = NetUserSetInfo(NULL, ETWPROXY_ACCOUNT, 1003, (LPBYTE)&ui, NULL);
    if (ns != NERR_Success)
    {
        SecureZeroMemory(pw, sizeof(pw));
        if (ns == NERR_UserNotFound)
            *parkReason = "account " "qubes-etwproxy" " absent - install-time provisioning "
                          "skipped or refused on this image";
        else
            *parkReason = "in-memory password reset refused (NetUserSetInfo 1003)";
        *parkCode = (DWORD)ns;
        return NULL;
    }

    // BATCH is the only logon type provisioning leaves this account: interactive,
    // remote-interactive and network are all denied (sec 10.14.2). This call is the
    // validation - it proves the password just set AND the batch right, milliseconds
    // before the token is used, with no storage in between.
    if (!LogonUserW(ETWPROXY_ACCOUNT, L".", pw, LOGON32_LOGON_BATCH, LOGON32_PROVIDER_DEFAULT, &token))
    {
        *parkCode = GetLastError();
        token = NULL;
        *parkReason = "batch logon refused for a password set milliseconds ago - "
                      "SeBatchLogonRight missing (provisioning predates the no-PLU contract?) "
                      "or the account is disabled/locked out";
    }
    SecureZeroMemory(pw, sizeof(pw));
    return token;
}

// ---- token census + consumer SID -----------------------------------------------------
// The never-held invariant, verified by inspection (design decision 2026-09-05): the
// consumer token must contain NEITHER the Performance Log Users group SID nor
// SeSystemProfilePrivilege at any instant - group membership is SE_GROUP_MANDATORY and
// cannot be shed later, so presence here means provisioning drift (an old script added
// PLU). SECURE DEFAULT (refuse-on-drift, 2026-09-05, replaces the earlier
// WARN-and-proceed): a drifted token would hand the untrusted TDH decode machine-wide
// trace capability, which is forbidden - *drifted is set and the caller PARKS the ETW
// tier for the boot (fail-open: the bridge degrades to listener/DB) instead of
// launching. The proxy's own census independently refuses the same condition (exit 9),
// belt-and-braces. Also copies the TokenUser SID out for the EventAccessControl grant.
// Returns FALSE only if the SID cannot be read at all.
static BOOL EtwProxyCensusAndSid(HANDLE token, BYTE* sidBuf, DWORD sidBufBytes, BOOL* drifted)
{
    union { TOKEN_USER tu; BYTE raw[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]; } tu;
    DWORD len = 0;
    *drifted = FALSE;
    if (!GetTokenInformation(token, TokenUser, &tu, sizeof(tu), &len))
        return FALSE;
    DWORD need = GetLengthSid(tu.tu.User.Sid);
    if (need > sidBufBytes || !CopySid(sidBufBytes, (PSID)sidBuf, tu.tu.User.Sid))
        return FALSE;

    BOOL pluPresent = FALSE, adminPresent = FALSE, profilePresent = FALSE;
    DWORD groupCount = 0, privCount = 0;

    BYTE plu[SECURITY_MAX_SID_SIZE], adm[SECURITY_MAX_SID_SIZE];
    DWORD cb = sizeof(plu);
    BOOL havePlu = CreateWellKnownSid(WinBuiltinPerfLoggingUsersSid, NULL, plu, &cb);
    cb = sizeof(adm);
    BOOL haveAdm = CreateWellKnownSid(WinBuiltinAdministratorsSid, NULL, adm, &cb);

    len = 0;
    GetTokenInformation(token, TokenGroups, NULL, 0, &len);
    if (len)
    {
        PTOKEN_GROUPS tg = (PTOKEN_GROUPS)LocalAlloc(LMEM_FIXED, len);
        if (tg && GetTokenInformation(token, TokenGroups, tg, len, &len))
        {
            groupCount = tg->GroupCount;
            for (DWORD i = 0; i < tg->GroupCount; i++)
            {
                if (havePlu && EqualSid(tg->Groups[i].Sid, (PSID)plu))
                    pluPresent = TRUE;
                if (haveAdm && EqualSid(tg->Groups[i].Sid, (PSID)adm))
                    adminPresent = TRUE;
            }
        }
        if (tg) LocalFree(tg);
    }

    LUID profLuid = { 0 };
    // Literal, not SE_SYSTEM_PROFILE_NAME: the TEXT() macro there follows the UNICODE
    // define, and this file calls the W API explicitly throughout.
    BOOL haveLuid = LookupPrivilegeValueW(NULL, L"SeSystemProfilePrivilege", &profLuid);
    len = 0;
    GetTokenInformation(token, TokenPrivileges, NULL, 0, &len);
    if (len)
    {
        PTOKEN_PRIVILEGES tp = (PTOKEN_PRIVILEGES)LocalAlloc(LMEM_FIXED, len);
        if (tp && GetTokenInformation(token, TokenPrivileges, tp, len, &len))
        {
            privCount = tp->PrivilegeCount;
            for (DWORD i = 0; haveLuid && i < tp->PrivilegeCount; i++)
                if (tp->Privileges[i].Luid.LowPart == profLuid.LowPart &&
                    tp->Privileges[i].Luid.HighPart == profLuid.HighPart)
                    profilePresent = TRUE;
        }
        if (tp) LocalFree(tp);
    }

    if (!g_CensusLogged)
    {
        g_CensusLogged = TRUE;
        LogInfo("ETWPROXYSUP consumer token census: groups=%lu privs=%lu plu=%d admin=%d "
                "se_system_profile=%d (invariant: plu=0 se_system_profile=0 - the token must "
                "never have held either)", groupCount, privCount, pluPresent, adminPresent,
                profilePresent);
    }
    *drifted = (pluPresent || profilePresent || adminPresent);
    if (*drifted)
        LogWarning("ETWPROXYSUP PROVISIONING DRIFT: consumer token holds %S%S%S- the "
                   "capability-grant split requires a bare account (no Performance Log Users, "
                   "no SeSystemProfilePrivilege); a drifted token has machine-wide trace "
                   "capability the untrusted decode must never hold, and group SIDs cannot "
                   "be shed. REFUSING the launch (ETW tier parked for this boot, fail-open). "
                   "Re-run guest/provision-etwproxy-account.ps1 from the current package",
                   pluPresent ? "Performance-Log-Users " : "",
                   profilePresent ? "SeSystemProfilePrivilege " : "",
                   adminPresent ? "Administrators " : "");
    return TRUE;
}

// ---- client SID: the console user the bridge runs as (pipe DACL principal) -----------
static BOOL EtwProxyClientSid(WCHAR* buf, size_t cch)
{
    DWORD session = WTSGetActiveConsoleSessionId();
    if (session == 0xFFFFFFFF)
        return FALSE;
    HANDLE utok = NULL;
    if (!WTSQueryUserToken(session, &utok))   // SYSTEM-only (SE_TCB) - exactly who we are
        return FALSE;
    union { TOKEN_USER tu; BYTE raw[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]; } ti;
    DWORD len = 0;
    BOOL ok = GetTokenInformation(utok, TokenUser, &ti, sizeof(ti), &len);
    CloseHandle(utok);
    if (!ok)
        return FALSE;
    LPWSTR s = NULL;
    if (!ConvertSidToStringSidW(ti.tu.User.Sid, &s))
        return FALSE;
    ok = SUCCEEDED(StringCchCopyW(buf, cch, s));
    LocalFree(s);
    return ok;
}

// ---- the sec 10.14.4 sandbox ---------------------------------------------------------
static HANDLE EtwProxyBuildJob(void)
{
    HANDLE job = CreateJobObjectW(NULL, NULL);   // anonymous: nothing can open it by name
    if (!job)
        return NULL;

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION ext;
    ZeroMemory(&ext, sizeof(ext));
    ext.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_ACTIVE_PROCESS
                                         | JOB_OBJECT_LIMIT_PROCESS_MEMORY
                                         | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    ext.BasicLimitInformation.ActiveProcessLimit = 1;   // no children, ever
    ext.ProcessMemoryLimit = ETWPROXY_MEM_LIMIT;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &ext, sizeof(ext)))
    {
        CloseHandle(job);
        return NULL;
    }

    JOBOBJECT_BASIC_UI_RESTRICTIONS ui;
    ui.UIRestrictionsClass = JOB_OBJECT_UILIMIT_DESKTOP
                           | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS
                           | JOB_OBJECT_UILIMIT_EXITWINDOWS
                           | JOB_OBJECT_UILIMIT_GLOBALATOMS
                           | JOB_OBJECT_UILIMIT_HANDLES
                           | JOB_OBJECT_UILIMIT_READCLIPBOARD
                           | JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS
                           | JOB_OBJECT_UILIMIT_WRITECLIPBOARD;
    if (!SetInformationJobObject(job, JobObjectBasicUIRestrictions, &ui, sizeof(ui)))
    {
        CloseHandle(job);
        return NULL;
    }
    return job;
}

// ---- launch (g_Lock held). On success: EPS_RUNNING + exit-wait registered. -----------
// On a park condition: EPS_PARKED (one log line). On a transient miss (no console user
// yet): stays EPS_IDLE for the next Poke. Never throws the agent off its loop: every
// call here is bounded (NetUserSetInfo/LogonUser/StartTrace/EnableTraceEx2/
// EventAccessControl/CreateProcessAsUser are all local control calls), runs at most once
// per backoff period, and typically once per boot.
//
// The privilege flow per launch (the 2026-09-05 architecture decision, exactly):
//   1. AGENT as SYSTEM: EventAccessControl SET(SYSTEM)+ADD(consumer realtime+query) FIRST,
//      THEN StartTraceW (fixed GUID) so the freshly-created logger snapshots the granted
//      SD, THEN EnableTraceEx2 per provider (RCs logged). Grant-before-start is
//      load-bearing (a real-time logger takes its SD at CREATION; a late grant edits only
//      the persisted SD and never reaches the running logger) AND the mask must include
//      WMIGUID_QUERY beside TRACELOG_ACCESS_REALTIME (ProcessTrace's implicit session
//      query) - each omission alone was a rig-measured exit 5.
//   2. AGENT: in-memory credential set + validate -> primary token with NO PLU and NO
//      SeSystemProfilePrivilege (census-verified, logged once); CreateProcessAsUserW
//      (CREATE_SUSPENDED|CREATE_NO_WINDOW, NO lpDesktop - the console split: etwproxy.exe
//      links no user32/gdi32, so no window station exists, is needed, or is granted) ->
//      AssignProcessToJobObject -> ResumeThread.
//   3. PROXY (etwproxy.exe): never-SYSTEM guard, --client-sid validation,
//      AdjustTokenPrivileges(SE_PRIVILEGE_REMOVED) on everything, then OpenTraceW +
//      ProcessTrace only - proxy-side, in tools/notifhost/etwproxy.cpp.
static void EtwProxyTryLaunchLocked(void)
{
    if (g_State != EPS_IDLE || g_Shutdown)
        return;

    WCHAR clientSid[RTL_NUMBER_OF(g_ClientSid)];
    if (!EtwProxyClientSid(clientSid, RTL_NUMBER_OF(clientSid)))
        return;   // no console user session yet - Poke retries on the existing loop pass

    // (2 first half) credentials: generate + set + prove, in memory only.
    const char* parkReason = NULL;
    DWORD parkCode = 0;
    HANDLE token = EtwProxyLogon(&parkReason, &parkCode);
    if (!token)
    {
        EtwProxyParkLocked(parkReason, parkCode);
        return;
    }

    // SESSION PLACEMENT: the proxy runs in THIS agent's session (the interactive/console
    // session a LogonUserW token inherits). An earlier fix stamped the token to session 0
    // (SetTokenInformation TokenSessionId=0) for a "more isolated" placement, but the rig
    // proved (2026-09-06 p3a run) that a session-0 process cannot be assigned to the
    // agent's (interactive-session) job object - AssignProcessToJobObject returns
    // ERROR_ACCESS_DENIED (5) and the proxy parks, tier down. The console-session proxy
    // works end to end (WMIGUID run: LIVE + payload-free SIG frames, privileges shed), its
    // security comes from the never-PLU/never-SYSTEM shed token NOT its session, and the
    // harness finds it by image name (T1j/T5 no longer session-filter). So the stamp is
    // REVERTED: it bought nothing and broke the job-sandbox assignment. Do not re-add it
    // without moving the job object to session 0 too.

    // Census (never-held invariant, logged) + the consumer SID the grant needs.
    BYTE consumerSid[SECURITY_MAX_SID_SIZE];
    BOOL drifted = FALSE;
    if (!EtwProxyCensusAndSid(token, consumerSid, sizeof(consumerSid), &drifted))
    {
        DWORD gle = GetLastError();
        CloseHandle(token);
        EtwProxyParkLocked("could not read the consumer token's SID for the session grant", gle);
        return;
    }
    if (drifted)
    {
        // SECURE DEFAULT (refuse-on-drift): a token carrying Performance Log Users /
        // SeSystemProfilePrivilege / Administrators must never run the hostile TDH decode
        // - it has machine-wide trace capability that cannot be shed in-process. Parked
        // BEFORE any session control, so nothing is started that nothing will consume.
        CloseHandle(token);
        EtwProxyParkLocked("consumer token DRIFTED (Performance Log Users / "
                           "SeSystemProfilePrivilege / Administrators present) - refusing to "
                           "run the untrusted decode with machine-wide trace capability; "
                           "re-run guest/provision-etwproxy-account.ps1 from the current "
                           "package", ERROR_ACCESS_DENIED);
        return;
    }

    // NO winsta preparation here (the 2026-09-05 console split): etwproxy.exe imports no
    // user32/gdi32, so nothing in its process init ever connects to a window station -
    // the 0xC0000142 failure that used to require the dedicated QubesEtwProxyWS is
    // structurally impossible, and no station is created or ACEd for the consumer.

    HANDLE job = EtwProxyBuildJob();
    if (!job)
    {
        // Refusing to run the untrusted parse OUTSIDE the sandbox is the point of the
        // design - an unsandboxed launch is not a fallback (sec 10.14.4).
        CloseHandle(token);
        EtwProxyParkLocked("job-object sandbox could not be built - refusing an unsandboxed launch",
                           GetLastError());
        return;
    }

    // (1) The controller sequence. CAPABILITY GRANT FIRST, then StartTrace, then enable.
    //
    // TWO STACKED DEFECTS, both rig-measured as the same exit-5 signature:
    //   a. ORDERING (fixed 2026-09-05, agent ff1c787): a real-time ETW logger snapshots its
    //      security descriptor from the persisted WMI\Security[Wnode.Guid] SD at the instant
    //      StartTraceW creates it. The original order (StartTrace -> EnableProviders ->
    //      grant) issued the EventAccessControl grant AFTER the logger existed, so the
    //      consumer's ACE landed only in the persisted registry SD (effective for the NEXT
    //      start) and never in the running logger. Grant-before-start stays load-bearing.
    //   b. MASK (found 2026-09-06, after ff1c787 alone STILL failed exit-5 with build
    //      identity proven): the ordering fix granted only TRACELOG_ACCESS_REALTIME, but
    //      the consumer-side realtime path also performs an implicit
    //      EVENT_TRACE_CONTROL_QUERY on the session, gated by WMIGUID_QUERY - see
    //      EtwCtlGrantConsumer's comment for the citation chain. The 09-05 comment here
    //      claimed the ordering was THE root cause; that claim was refuted on the rig and
    //      is retracted - ordering was necessary, not sufficient.
    // The census had already proven the token held neither PLU nor SeSystemProfilePrivilege
    // (else exit 9), and per MSDN the per-session EventAccessControl grant IS the documented
    // non-PLU route to realtime consumption, so the never-PLU / never-SYSTEM consumer
    // architecture holds. (design 10.16.3b / 10.20.6 follow-up.)
    ULONG rc = EtwCtlGrantConsumer((PSID)consumerSid);
    if (rc != ERROR_SUCCESS)
    {
        CloseHandle(job); CloseHandle(token);
        // No session is running yet; EtwProxyBackoffLocked's StopLocked is a no-op
        // (g_SessLive is still FALSE). SYSTEM being denied WRITE_DAC on its own session GUID
        // would mean the trace config is broken, but that is vanishingly unlikely for SYSTEM;
        // treat any grant failure as transient and retry on backoff.
        LogWarning("ETWPROXYSUP EventAccessControl grant failed (%lu) - consumer not authorized; "
                   "will retry on backoff", rc);
        EtwProxyBackoffLocked();
        return;
    }
    TRACEHANDLE sess = 0;
    rc = EtwCtlSessionStart(&sess);   // reaps stale, then StartTraceW -> reads the SD just set
    LogInfo("PROXY RC api=StartTrace provider=- rc=%lu", rc);
    if (rc != ERROR_SUCCESS)
    {
        CloseHandle(job); CloseHandle(token);
        // SYSTEM being denied session control means the machine's trace config is broken
        // in a way a relaunch cannot fix; anything else gets the backoff retry.
        if (rc == ERROR_ACCESS_DENIED)
            EtwProxyParkLocked("StartTrace access-denied for SYSTEM - trace configuration broken", rc);
        else
        {
            LogWarning("ETWPROXYSUP StartTrace failed (%lu) - will retry on backoff", rc);
            EtwProxyBackoffLocked();
        }
        return;
    }
    g_SessLive = TRUE;
    int enabled = EtwCtlEnableProviders(sess);
    if (enabled == 0)
    {
        CloseHandle(job); CloseHandle(token);
        LogWarning("ETWPROXYSUP zero providers enabled - will retry on backoff");
        EtwProxyBackoffLocked();   // stops the session
        return;
    }

    // (2 second half) launch the consumer inside the sandbox.
    WCHAR exe[MAX_PATH];
    if (!GetModuleFileNameW(NULL, exe, RTL_NUMBER_OF(exe)))
    {
        CloseHandle(job); CloseHandle(token);
        EtwProxySessionStopLocked();
        return;
    }
    WCHAR* slash = wcsrchr(exe, L'\\');
    if (slash) *(slash + 1) = 0;   // keep trailing backslash -> agent's own bin directory

    WCHAR cmd[MAX_PATH + 256];
    // etwproxy.exe ships next to gui-agent.exe (bin overlay), like notifhost/wgcbroker.
    // It IS the proxy - no mode flag needed; --client-sid is its whole command line.
    if (FAILED(StringCchPrintfW(cmd, RTL_NUMBER_OF(cmd),
                                L"\"%setwproxy.exe\" --client-sid %s",
                                exe, clientSid)))
    {
        CloseHandle(job); CloseHandle(token);
        EtwProxySessionStopLocked();
        return;
    }

    // No LoadUserProfile (the proxy needs no HKCU, sec 10.14.4); environment from the
    // proxy token so ProgramData etc. resolve (etwproxy falls back to C:\ProgramData
    // if they do not - env is comfort, not correctness).
    LPVOID env = NULL;
    (void)CreateEnvironmentBlock(&env, token, FALSE);

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    // si.lpDesktop stays NULL (the console split). Historical note: `lpDesktop = L""`
    // then a dedicated winsta path were both about user32's process-init winsta connect,
    // which etwproxy.exe (no user32/gdi32 imports) never performs - the field is inert
    // for it, and naming any station here would only re-couple the launch to winsta
    // state that no longer needs to exist.

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    // CREATE_SUSPENDED -> assign to job -> resume: the proxy never runs outside the job.
    BOOL created = CreateProcessAsUserW(token, NULL, cmd, NULL, NULL, FALSE,
                                        CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                                        env, NULL, &si, &pi);
    DWORD createGle = created ? 0 : GetLastError();
    if (env)
        DestroyEnvironmentBlock(env);
    CloseHandle(token);
    if (!created)
    {
        CloseHandle(job);
        LogWarning("ETWPROXYSUP CreateProcessAsUser failed (%lu) - will retry on backoff", createGle);
        EtwProxyBackoffLocked();   // stops the session
        return;
    }

    if (!AssignProcessToJobObject(job, pi.hProcess))
    {
        // Suspended, never ran, and must not: kill it rather than run unsandboxed.
        DWORD gle = GetLastError();
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CloseHandle(job);
        EtwProxyParkLocked("could not assign the proxy to its job sandbox", gle);
        return;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    if (!RegisterWaitForSingleObject(&g_Wait, pi.hProcess, EtwProxyExitCb, NULL,
                                     INFINITE, WT_EXECUTEONLYONCE))
    {
        // Without the exit-wait there is no supervision at all - tear down and park
        // rather than leave an unsupervised privileged-ish process behind.
        DWORD gle = GetLastError();
        TerminateJobObject(job, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(job);
        EtwProxyParkLocked("RegisterWaitForSingleObject failed - no exit-wait, no launch", gle);
        return;
    }

    g_Job = job;
    g_Proc = pi.hProcess;
    g_LaunchTick = GetTickCount64();
    StringCchCopyW(g_ClientSid, RTL_NUMBER_OF(g_ClientSid), clientSid);
    g_State = EPS_RUNNING;
    LogInfo("ETWPROXYSUP launched etwproxy.exe pid=%lu client_sid=%s "
            "(session controller: %s live, providers=%d, consumer granted "
            "TRACELOG_ACCESS_REALTIME|WMIGUID_QUERY; job: 64MB/1-proc/UI-restricted/"
            "kill-on-close; agent session, no winsta - GUI-DLL-free console proxy; "
            "exit-wait armed)",
            pi.dwProcessId, g_ClientSid, ETWPROXY_SESSION_NAME, enabled);
}

// ---- exit-wait callback (thread pool): THE supervision mechanism ---------------------
static VOID CALLBACK EtwProxyExitCb(PVOID context, BOOLEAN timedOut)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(timedOut);   // INFINITE wait: only ever fires on process exit

    EnterCriticalSection(&g_Lock);
    if (g_Shutdown || g_State != EPS_RUNNING)
    {
        LeaveCriticalSection(&g_Lock);
        return;
    }

    DWORD rc = (DWORD)-1;
    GetExitCodeProcess(g_Proc, &rc);
    ULONGLONG uptimeMs = GetTickCount64() - g_LaunchTick;

    if (g_Wait)
    {
        UnregisterWaitEx(g_Wait, NULL);   // non-blocking: we ARE the callback
        g_Wait = NULL;
    }
    CloseHandle(g_Proc);
    g_Proc = NULL;
    CloseHandle(g_Job);   // process already gone; KILL_ON_JOB_CLOSE has nothing left to kill
    g_Job = NULL;

    if (rc == ETWPROXY_EXIT_DENIED)
    {
        // OpenTrace/consume access-denied UNDER THE GRANT: even the realtime+query pair on
        // the session GUID is not sufficient for a bare-token consumer on this build -
        // the sec 10.16.3b datum as redefined by the split. A relaunch cannot change
        // rights; record the finding and stand down (park also stops the session).
        // Next diagnostic rung if this ever fires again: widen the ADD ACE to the
        // PLU-equivalent session subset (add WMIGUID_NOTIFICATION) to decide bit-vs-
        // structural before any owner escalation.
        EtwProxyParkLocked("proxy exit 5: consume access-denied despite the per-session DACL "
                           "grant - TRACELOG_ACCESS_REALTIME|WMIGUID_QUERY insufficient on "
                           "this build (a FINDING, see design sec 10.16.3b)", rc);
        LeaveCriticalSection(&g_Lock);
        return;
    }
    if (rc == ETWPROXY_EXIT_BADTOKEN)
    {
        // The proxy refused its own token: either the never-SYSTEM/admin guard tripped
        // (this launch path handed it a privileged token - plumbing broken) or its census
        // found drift (PLU / SeSystemProfilePrivilege) the agent-side census should have
        // caught first. Both are rights problems a relaunch cannot fix.
        EtwProxyParkLocked("proxy exit 9: it refused the launch token (never-SYSTEM guard "
                           "or token-drift census) - investigate the launch plumbing / "
                           "account provisioning before rearming", rc);
        LeaveCriticalSection(&g_Lock);
        return;
    }

    if (uptimeMs > ETWPROXY_HEALTHY_MS)
        g_Backoff = ETWPROXY_BACKOFF_MIN;   // it ran healthily; treat this exit as fresh

    // The 'proxy exited rc=<n> after' prefix is a GREP CONTRACT with the p3a gate
    // (t2-proxy-exits collection + the T8c 'proxy exited rc=8 after' detector) - the
    // decode/anomaly text goes AFTER it, never inside it.
    if (rc == 0 || rc == ETWPROXY_EXIT_CONSUME || rc == ETWPROXY_EXIT_PIPE)
    {
        LogWarning("ETWPROXYSUP proxy exited rc=%lu after %llu ms (%S) - relaunch in %lu ms",
                   rc, uptimeMs,
                   rc == 0 ? "clean stop - session ended externally or console ctrl" :
                   rc == ETWPROXY_EXIT_CONSUME ? "consumer open/thread failure" :
                   "pipe failure: squatter or persistent connect faults",
                   g_Backoff);
    }
    else if (rc == ETWPROXY_EXIT_KILLED)
    {
        // rc=1 is the code TerminateProcess IMPOSES - taskkill /f, Stop-Process -Force, or
        // our own stop-by-name reap. It is NOT a proxy return path, but it is EXPECTED on any
        // supervised kill (park/shutdown/job-kill, the next-launch reap, the p3a T5/T8c
        // drills). Log it quietly + machine-readably (its own token, still greppable) and
        // relaunch; do NOT raise EtwProxyUnknownExit, which stays reserved for a TRULY
        // unaccounted code (a crash) so a benign external kill cannot desensitize it.
        LogWarning("ETWPROXYSUP proxy exited rc=%lu after %llu ms (external force-termination: "
                   "TerminateProcess/taskkill/Stop-Process/reap - expected on a supervised "
                   "kill, not a proxy return path) - relaunch in %lu ms",
                   rc, uptimeMs, g_Backoff);
    }
    else
    {
        // NOT a code the proxy can return (0/5/7/8/9) NOR the force-kill code (1): an
        // uncommanded crash. A fallback firing silently is how defects hide - log the anomaly
        // loudly and machine-readably, then still relaunch (the tier is not worth wedging
        // over, but the datum must not vanish into a routine line).
        LogWarning("ETWPROXYSUP proxy exited rc=%lu after %llu ms - ANOMALY "
                   "EtwProxyUnknownExit (0x%lX): not an ETWPROXY_EXIT_* code (0/5/7/8/9) and "
                   "not the force-kill code 1 - the proxy binary has no such return path, so "
                   "it CRASHED (check etw-proxy.log for a CRASH line). Diagnose before "
                   "trusting the tier; relaunch in %lu ms",
                   rc, uptimeMs, rc, g_Backoff);
    }
    EtwProxyBackoffLocked();   // stops the session; the relaunch restarts it fresh
    LeaveCriticalSection(&g_Lock);
}

// ---- backoff timer callback (thread pool): relaunch ----------------------------------
static VOID CALLBACK EtwProxyRelaunchCb(PVOID context, BOOLEAN timerFired)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(timerFired);

    EnterCriticalSection(&g_Lock);
    if (g_Timer)
    {
        // Deleting our own one-shot timer with a NULL completion event is the
        // documented in-callback cleanup; it cannot deadlock and frees the timer.
        DeleteTimerQueueTimer(NULL, g_Timer, NULL);
        g_Timer = NULL;
    }
    if (!g_Shutdown && g_State == EPS_BACKOFF)
    {
        g_State = EPS_IDLE;
        EtwProxyTryLaunchLocked();   // no console user yet -> stays IDLE, Poke picks it up
    }
    LeaveCriticalSection(&g_Lock);
}

// ---- public API (see etwproxy.h) -----------------------------------------------------

void EtwProxyInit(BOOL bridgeEnabled)
{
    InitializeCriticalSection(&g_Lock);
    g_Inited = TRUE;
    g_Enabled = bridgeEnabled;
    g_State = bridgeEnabled ? EPS_IDLE : EPS_DISABLED;
    if (bridgeEnabled)
        LogInfo("ETWPROXYSUP armed (gate on): etwproxy.exe launches once a console "
                "user session exists (agent = session controller, proxy = pure consumer)");
    // Gate off: fully inert. No account access, no session, no logs, no timers.
}

void EtwProxyPoke(void)
{
    if (!g_Inited || !g_Enabled)
        return;
    // Main-loop thread only; cheap early-out before the lock.
    ULONGLONG now = GetTickCount64();
    if (now < g_NextPoke)
        return;
    g_NextPoke = now + ETWPROXY_POKE_MS;

    EnterCriticalSection(&g_Lock);
    if (!g_Shutdown)
    {
        if (g_State == EPS_IDLE)
            EtwProxyTryLaunchLocked();
        else if (g_State == EPS_RUNNING)
        {
            // Console user changed: the pipe DACL admits exactly the launched SID, so a
            // proxy serving the OLD user is useless to the NEW user's bridge. Kill the
            // job; the exit-wait relaunches with the fresh SID (backoff reset - this is
            // a deliberate restart, not a crash). The relaunch also restarts the session
            // and re-issues the grant, so nothing stale survives the user change.
            WCHAR sid[RTL_NUMBER_OF(g_ClientSid)];
            if (EtwProxyClientSid(sid, RTL_NUMBER_OF(sid)) && wcscmp(sid, g_ClientSid) != 0)
            {
                LogInfo("ETWPROXYSUP console user changed (%s -> %s) - restarting proxy for the new client SID",
                        g_ClientSid, sid);
                g_Backoff = ETWPROXY_BACKOFF_MIN;
                TerminateJobObject(g_Job, 0);
            }
        }
    }
    LeaveCriticalSection(&g_Lock);
}

void EtwProxyShutdown(void)
{
    if (!g_Inited)
        return;

    // Phase 1: flag + detach the async sources under the lock.
    EnterCriticalSection(&g_Lock);
    g_Shutdown = TRUE;
    HANDLE wait = g_Wait;   g_Wait = NULL;
    HANDLE timer = g_Timer; g_Timer = NULL;
    LeaveCriticalSection(&g_Lock);

    // Phase 2: blocking drains OUTSIDE the lock (the callbacks take it; holding it
    // here would deadlock). After these return no callback is running or pending.
    if (wait)
        UnregisterWaitEx(wait, INVALID_HANDLE_VALUE);
    if (timer)
        DeleteTimerQueueTimer(NULL, timer, INVALID_HANDLE_VALUE);

    // Phase 3: tear the proxy down, then the session (the agent owns its lifecycle now:
    // stop at shutdown, stale reap at next start covers the crash path). TerminateJobObject
    // covers the process and (with KILL_ON_JOB_CLOSE) agent-crash cleanup was always
    // covered; a session an agent CRASH leaks is reaped by the next launch's
    // stop-by-name-first (EtwCtlSessionStart).
    EnterCriticalSection(&g_Lock);
    if (g_Job)
    {
        TerminateJobObject(g_Job, 0);
        CloseHandle(g_Job);
        g_Job = NULL;
    }
    if (g_Proc)
    {
        CloseHandle(g_Proc);
        g_Proc = NULL;
    }
    // (No winsta/desktop refs to drop since the console split - the proxy owns no
    // window-station objects and the agent creates none for it.)
    EtwProxySessionStopLocked();
    g_State = g_Enabled ? EPS_IDLE : EPS_DISABLED;
    LeaveCriticalSection(&g_Lock);
}
