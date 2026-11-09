// WUM-Mod — Worms Ultimate Mayhem (Steam, build 1077) : équipes IA dans les parties en ligne,
// réglages d'équipe en ligne (vers, alliés, handicap) et Wormpot réglé par l'hôte seul.
// Plugin .asi chargé par Ultimate ASI Loader ; l'exe n'est pas modifié sur disque (tout en mémoire).
//
// Fonctionnement :
//  - Lobby (NetService, NetService_Xbox.cpp) : l'hôte clique une ligne libre → liste des équipes CPU
//    de sa caserne ; une ligne occupée → fenêtre locale de réglages d'équipe (WXFEP.TeamSettings).
//  - L'hôte envoie l'état aux invités dans des TeamMessage marqués (champ Player) :
//    « WUM#k/n#pseudoHôte#équipe » (IA), « WUMWP#r1#r2#r3 » (Wormpot), « WUMTS#… » (réglages).
//  - Au lancement, chaque PC exécute NetService::ProcessBeginGameMessage (0x70933A) ; le mod y ajoute
//    les mêmes IA dans le même ordre (Player = celui de l'hôte, Skill > 0 → IA) puis applique les
//    réglages d'équipe dans GM.GameInitData. L'IA est calculée sur chaque PC : sa réflexion est rendue
//    indépendante du nombre d'images par seconde (déterminisme réseau).
//  - Lobby affiché comme la liste locale : lignes occupées reconstruites (couleur, handicap, vers).
//
// Compilation : build.bat (MinGW 32 bits). -DWUM_NOLOG : version finale sans journal.
// Version de diagnostic (sans -DWUM_NOLOG) : journal wummod_log.txt à la racine du jeu.

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>

#define WUM_VERSION "WUM-Mod v40"

// ---------------------------------------------------------------- adresses (build 1077)
#define NS_VT         0x89B3ECu  // vtable NetService (+0x18 Update)
#define NS_INSTANCE   0x979DDCu  // NetService*
#define NS_SEND       0x70621Du  // NetService::Send(msg) thiscall ret 4
#define NS_SEND_TEAMS 0x70B061u  // adresse de retour : envoi des équipes au lancement
#define NS_BEGINGAME  0x70933Au  // NetService::ProcessBeginGameMessage(msg) thiscall ret 4
#define NS_REFRESH    0x706687u  // NetService::RefreshLobby(force) thiscall ret 4
#define NS_ISHOST     0x4ACu     // ressource WXNET.LocalPlayerIsHost
#define F_SET_STRING  0x50BB80u  // HRESULT cdecl (const char** nom, const char* valeur)
#define F_SET_BOOL    0x50BA00u  // HRESULT cdecl (const char** nom, int valeur)
#define TEAMMSG_VT    0x883680u  // vtable TeamMessage (+0x18 Handle)
#define TEAMMSG_CLASS 0x882D68u  // descripteur de classe TeamMessage
#define CLONER_CLASS  0x893580u  // assistant de copie de conteneur (utilisé par ProcessBeginGame)
#define TC_VT         0x8A19B4u  // vtable TeamControlService (+0x20 AddTeamToGame)
#define TC_INSTANCE   0x97AA88u
#define TC_LOCALFLAG  0x97AA84u  // octet posé avant AddTeamToGame (joueur+0x4E)
#define MX_VT         0x871608u  // vtable du multiplexeur de log
#define DBG_CFG_PTR   0x95A100u  // objet de config ; +0x9A bit 2 = traces debug du jeu

#define F_CREATE      0x639B83u  // void* cdecl CreateInstance(desc)
#define F_SPTR_SET    0x78E7F0u  // thiscall smartptr::operator=(obj) (AddRef)
#define F_SPTR_SET2   0x6F7C0Bu  // thiscall smartptr::operator=(obj) (AddRef)
#define F_SETREF      0x441F30u  // cdecl SetRefField(obj, ref, offset)
#define F_XSTR_NEW    0x638101u  // thiscall XString::XString(const char*)
#define F_XSTR_SET    0x441EA0u  // thiscall XString::operator=(const XString&)
#define F_XSTR_DEL    0x4030A7u  // thiscall XString::~XString()
#define F_RES_FIND    0x50B760u  // HRESULT cdecl (const char** nom, void** res)
#define F_GET_STR     0x50B820u  // HRESULT cdecl (const char** nom, const char** out)
#define F_GET_BOOL    0x50B790u  // HRESULT cdecl (const char** nom, int* out)
#define F_PL_COUNT    0x67CB62u  // thiscall PlayerList::Count()
#define F_PL_GET      0x67C982u  // thiscall PlayerList::Get(i)

// joueur réseau
#define PL_NICK 0x14
#define PL_TEAM 0x44
#define PL_FLAG 0x4E
// équipe (TeamData)
#define TEAM_NAME   0x14
#define TEAM_SKILL  0x20
#define TEAM_PLAYER 0x34
// GameInitData
#define GI_NUMTEAMS 0x14
#define GI_ISLOCAL  0x18C        // équipes : voir gi_team() (décalages non réguliers)

#define MAX_AI 3
#define MARK "WUM#"

// ---------------------------------------------------------------- log
static char g_dir[MAX_PATH];
static CRITICAL_SECTION g_cs;
#ifndef WUM_NOLOG
static FILE* g_log;
static int g_line_start = 1;

static void stamp(void) {
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
}
#endif

static void logf_(const char* fmt, ...) {
#ifndef WUM_NOLOG
    if (!g_log) return;
    EnterCriticalSection(&g_cs);
    if (!g_line_start) fputc('\n', g_log);
    stamp(); fputs("[WUM] ", g_log);
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log); g_line_start = 1;
    LeaveCriticalSection(&g_cs);
#endif
}

#ifndef WUM_NOLOG
// texte brut des canaux de log du jeu (peut arriver par morceaux)
static void log_raw(const char* s, int len) {
    if (!g_log || !s || len <= 0) return;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < len; i++) {
        char c = s[i];
        if (c == '\r') continue;
        if (g_line_start) { stamp(); fputs("| ", g_log); g_line_start = 0; }
        fputc(c, g_log);
        if (c == '\n') g_line_start = 1;
    }
    LeaveCriticalSection(&g_cs);
}
#endif

static void log_flush(void) {
#ifndef WUM_NOLOG
    if (!g_log) return;
    EnterCriticalSection(&g_cs); fflush(g_log); LeaveCriticalSection(&g_cs);
#endif
}

// ---------------------------------------------------------------- utilitaires mémoire
static int rdok(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION m;
    if ((uintptr_t)p < 0x10000) return 0;
    if (!VirtualQuery(p, &m, sizeof m)) return 0;
    if (m.State != MEM_COMMIT || (m.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    return (uintptr_t)p + n <= (uintptr_t)m.BaseAddress + m.RegionSize;
}
static uint32_t rd32(uintptr_t a) { return rdok((void*)a, 4) ? *(uint32_t*)a : 0; }
static uint8_t rd8(uintptr_t a) { return rdok((void*)a, 1) ? *(uint8_t*)a : 0; }
static const char* sstr(uint32_t p) { return rdok((void*)p, 1) ? (const char*)p : "(?)"; }

static int vt_patch(uintptr_t slot, void* detour, uintptr_t expect, void** orig, const char* name) {
    if (rd32(slot) != expect) {
        logf_("ERREUR crochet %s : vtable %08X = %08X (attendu %08X) -> desactive", name, (unsigned)slot, rd32(slot), (unsigned)expect);
        return 0;
    }
    DWORD old;
    VirtualProtect((void*)slot, 4, PAGE_READWRITE, &old);
    *orig = (void*)*(uintptr_t*)slot;
    *(void**)slot = detour;
    VirtualProtect((void*)slot, 4, old, &old);
    return 1;
}

// crochet en ligne : les octets volés doivent être des instructions entières sans adresse relative
static void* make_hook(uintptr_t target, void* detour, const uint8_t* sig, int stolen, const char* name) {
    if (!rdok((void*)target, stolen) || memcmp((void*)target, sig, stolen) != 0) {
        logf_("ERREUR crochet %s : signature differente en %08X -> desactive", name, (unsigned)target);
        return NULL;
    }
    uint8_t* tr = (uint8_t*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(tr, (void*)target, stolen);
    tr[stolen] = 0xE9;
    *(int32_t*)(tr + stolen + 1) = (int32_t)((target + stolen) - ((uintptr_t)tr + stolen + 5));
    DWORD old;
    VirtualProtect((void*)target, stolen, PAGE_EXECUTE_READWRITE, &old);
    uint8_t* t = (uint8_t*)target;
    t[0] = 0xE9;
    *(int32_t*)(t + 1) = (int32_t)((uintptr_t)detour - (target + 5));
    for (int k = 5; k < stolen; k++) t[k] = 0x90;
    VirtualProtect((void*)target, stolen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)target, stolen);
    return tr;
}

// ---------------------------------------------------------------- configuration (version de diagnostic)
static int g_debug_trace = 1;

static void load_ini(void) {
#ifdef WUM_NOLOG
    g_debug_trace = 0;
    return;
#endif
    char p[MAX_PATH]; snprintf(p, sizeof p, "%swummod.ini", g_dir);
    FILE* f = fopen(p, "r");
    int current = 0;
    if (f) { char l[256]; while (fgets(l, sizeof l, f)) if (strstr(l, "[Main] v40")) current = 1; fclose(f); }
    if (!current) {   // fichier absent ou ancien : réécrit
        f = fopen(p, "w");
        if (f) {
            fputs("; WUM-Mod (version de diagnostic) - [Main] v40\n"
                  "[Main]\n"
                  "; 1 = active les traces debug du jeu dans le log (reseau, tours, IA)\nDebugTrace=1\n", f);
            fclose(f);
        }
    }
    g_debug_trace = GetPrivateProfileIntA("Main", "DebugTrace", 1, p);
}

// ---------------------------------------------------------------- appels au moteur
typedef int (__cdecl *res_find_t)(const char**, void**);
typedef int (__cdecl *get_str_t)(const char**, const char**);
typedef int (__cdecl *get_int_t)(const char**, int*);
typedef void* (__cdecl *create_t)(uintptr_t);
typedef void (__cdecl *setref_t)(uintptr_t, uintptr_t, uintptr_t);
typedef uintptr_t (__fastcall *this0_t)(uintptr_t, uintptr_t);
typedef uintptr_t (__fastcall *this1_t)(uintptr_t, uintptr_t, uintptr_t);
typedef int (__stdcall *com1_t)(uintptr_t);
typedef int (__stdcall *com2_t)(uintptr_t, uintptr_t);
typedef int (__stdcall *com4_t)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
typedef int (__cdecl *set_str_t)(const char**, const char*);
typedef int (__cdecl *set_int_t)(const char**, int);

static uintptr_t vfn(uintptr_t obj, int off) { return rd32(rd32(obj) + off); }
static void obj_addref(uintptr_t o) { if (o && vfn(o, 4)) ((com1_t)vfn(o, 4))(o); }
static void obj_release(uintptr_t o) { if (o && vfn(o, 8)) ((com1_t)vfn(o, 8))(o); }

static void set_xstr(uintptr_t field, const char* s) {
    uintptr_t tmp = 0;
    ((this1_t)F_XSTR_NEW)((uintptr_t)&tmp, 0, (uintptr_t)s);
    ((this1_t)F_XSTR_SET)(field, 0, (uintptr_t)&tmp);
    ((this0_t)F_XSTR_DEL)((uintptr_t)&tmp, 0);
}

static const char* get_str(const char* name) {
    const char* n = name; const char* out = NULL;
    if (((get_str_t)F_GET_STR)(&n, &out) < 0 || !out) return "";
    return sstr((uintptr_t)out);
}

// NetService+0x4AC = ressource de données WXNET.LocalPlayerIsHost (valeur à *(*(res+4)+0x1C)),
// tenue à jour par le jeu depuis la session XomOnline (NetService+0x8C)->vt[0x34]().
// Game.HostIsLocal reste à 0 dans le lobby Steam.
// Attention : WXNET.LocalPlayerIsHost repasse à 0 pendant le décompte de lancement (c'est en fait
// « l'hôte peut modifier le lobby », recopié depuis NetService+0x4B0). Le vrai statut d'hôte vient de
// la session : (NetService+0x8C)->vt[0x34]() renvoie 1 pour l'hôte (cf. 0x70AC3C).
typedef char (__stdcall *ishost_t)(uintptr_t);
static int host_flag(uintptr_t ns) {
    uintptr_t sess = ns ? rd32(ns + 0x8C) : 0;
    uintptr_t fn = sess ? vfn(sess, 0x34) : 0;
    if (fn >= 0x401000 && fn < 0x815000) return ((ishost_t)fn)(sess) == 1;
    uintptr_t h = ns ? rd32(ns + NS_ISHOST) : 0;
    uintptr_t inner = h ? rd32(h + 4) : 0;
    return inner ? (int)rd32(inner + 0x1C) : -1;
}
static int host_is_local(void) { return host_flag(rd32(NS_INSTANCE)); }

// Ressource de données : conteneur (sans référence ajoutée) ; *res à libérer.
static uintptr_t res_container(const char* name, void** res) {
    *res = NULL;
    const char* n = name;
    int hr = ((res_find_t)F_RES_FIND)(&n, res);
    if (hr < 0 || !*res) { logf_("ressource %s introuvable (hr=%08X)", name, hr); return 0; }
    uintptr_t inner = rd32((uintptr_t)*res + 4);
    return inner ? rd32(inner + 0x1C) : 0;
}
static void res_release(void* res) { obj_release((uintptr_t)res); }

// copie d'une équipe, comme ProcessBeginGameMessage (référence ajoutée pour l'appelant)
static uintptr_t clone_team(uintptr_t src) {
    uintptr_t sp = 0;
    void* h = ((create_t)F_CREATE)(CLONER_CLASS);
    if (!h) { logf_("clone : creation de l'assistant impossible"); return 0; }
    ((this1_t)F_SPTR_SET)((uintptr_t)&sp, 0, (uintptr_t)h);
    if (!sp) return 0;
    ((com2_t)vfn(sp, 0x30))(sp, 2);
    ((com2_t)vfn(sp, 0x10))(sp, src);
    uintptr_t c = ((com1_t)vfn(sp, 0x38))(sp);
    obj_addref(c);
    obj_release(sp);
    return c;
}

static uintptr_t netservice(void) { return rd32(NS_INSTANCE); }
static uintptr_t ns_players(void) { uintptr_t ns = netservice(); return ns ? rd32(ns + 0x4A8) : 0; }
static int pl_count(uintptr_t pl) { return pl ? (int)((this0_t)F_PL_COUNT)(pl, 0) : 0; }
static uintptr_t pl_get(uintptr_t pl, int i) { return ((this1_t)F_PL_GET)(pl, 0, (uintptr_t)i); }

// joueur local = celui dont l'octet +0x4E est non nul ; pseudo Steam « ST:… » à +0x14
static uintptr_t local_player(void) {
    uintptr_t pl = ns_players();
    for (int i = 0, n = pl_count(pl); i < n; i++) {
        uintptr_t p = pl_get(pl, i);
        if (rd8(p + PL_FLAG)) return p;
    }
    return 0;
}
static const char* local_nick(void) { uintptr_t p = local_player(); return p ? sstr(rd32(p + PL_NICK)) : ""; }
static uintptr_t find_player(const char* nick) {
    uintptr_t pl = ns_players();
    for (int i = 0, n = pl_count(pl); i < n; i++) {
        uintptr_t p = pl_get(pl, i);
        if (strcmp(sstr(rd32(p + PL_NICK)), nick) == 0) return p;
    }
    return 0;
}

// ---------------------------------------------------------------- dumps
// position des équipes dans GameInitData (schéma : T1_Name 0x2C, T2 0x94, T3 0xF4, T4 0x14C) ;
// Name +0, Player +4, NumOfWorms +8, Skill +0x24
static uintptr_t gi_team(uintptr_t gi, int k) {
    static const uint16_t off[4] = {0x2C, 0x94, 0xF4, 0x14C};
    return gi + off[k & 3];
}
static void dump_gameinit(const char* why) {
    void* res;
    uintptr_t gi = res_container("GM.GameInitData", &res);
    if (gi) {
        int n = rd8(gi + GI_NUMTEAMS);
        logf_("GameInitData (%s) : %d equipe(s)", why, n);
        for (int k = 0; k < 4 && k < n; k++) {
            uintptr_t t = gi_team(gi, k);
            logf_("  T%d nom='%s' joueur='%s' vers=%d Skill=%d IsLocal=%d allies=%d handicap=%d", k + 1,
                  sstr(rd32(t)), sstr(rd32(t + 4)), (int)((this1_t)0x67C836u)(gi, 0, k), (int)rd32(t + 0x24),
                  rd8(gi + GI_ISLOCAL + k), (int)((this1_t)0x67C74Fu)(gi, 0, k), (int)((this1_t)0x67C798u)(gi, 0, k));
        }
    }
    res_release(res);
}

static int gameinit_has(const char* name) {
    void* res; int found = 0;
    uintptr_t gi = res_container("GM.GameInitData", &res);
    if (gi) {
        int n = rd8(gi + GI_NUMTEAMS);
        for (int k = 0; k < 4 && k < n; k++)
            if (strcmp(sstr(rd32(gi_team(gi, k))), name) == 0) found = 1;
    }
    res_release(res);
    return found;
}

static int gameinit_count(void) {
    void* res;
    uintptr_t gi = res_container("GM.GameInitData", &res);
    int n = gi ? rd8(gi + GI_NUMTEAMS) : -1;
    res_release(res);
    return n;
}

static void dump_players(const char* why) {
    uintptr_t pl = ns_players();
    int n = pl_count(pl);
    logf_("Joueurs (%s) : %d, hote local=%d, pseudo local='%s'", why, n, host_is_local(), local_nick());
    for (int i = 0; i < n; i++) {
        uintptr_t p = pl_get(pl, i), t = rd32(p + PL_TEAM);
        logf_("  [%d] '%s' drapeau4E=%d equipe='%s' (joueur='%s' Skill=%d)", i, sstr(rd32(p + PL_NICK)), rd8(p + PL_FLAG),
              t ? sstr(rd32(t + TEAM_NAME)) : "-", t ? sstr(rd32(t + TEAM_PLAYER)) : "-", t ? (int)rd32(t + TEAM_SKILL) : 0);
    }
}

static int barracks(uintptr_t* arr, int max, void** res) {
    uintptr_t coll = res_container("DATA.TeamBarracks", res);
    if (!coll) return 0;
    uintptr_t vec = rd32(coll + 0x14);
    int n = (int)rd32(vec + 0x18);
    if (n < 0 || n > 256) n = 0;
    int k = 0;
    for (int i = 0; i < n && k < max; i++) arr[k++] = rd32(vec + 0x20 + 4 * i);
    return k;
}

// ---------------------------------------------------------------- IA préparées (hôte) / reçues (invité)
// Marque transportée dans team.Player : « WUM#k/n#pseudoHôte#équipeSource » (k = emplacement, n = nombre d'IA)
typedef struct { uintptr_t team; int level; char src[64]; } ai_t;
static ai_t g_ai[MAX_AI];          // hôte : IA préparées (équipes clonées, propriété du mod)
static int g_ai_n;
static uintptr_t g_recv[MAX_AI];   // invité : équipes IA reçues de l'hôte
static char g_recv_src[MAX_AI][64];
static int g_recv_n;
static char g_recv_host[64];
static DWORD g_last_ai_send;
static uintptr_t g_ns_seen;

typedef int (__fastcall *thiscall1_t)(uintptr_t, uintptr_t, uintptr_t);
static thiscall1_t o_send, o_begingame, o_teammsg, o_refresh;

static void set_mark(uintptr_t team, int k, int n, const char* src) {
    char mark[160];
    snprintf(mark, sizeof mark, MARK "%d/%d#%s#%s", k, n, local_nick(), src);
    set_xstr(team + TEAM_PLAYER, mark);
}

static void send_team_msg_quiet(uintptr_t ns, uintptr_t team) {
    uintptr_t sp = 0;
    void* m = ((create_t)F_CREATE)(TEAMMSG_CLASS);
    if (!m) return;
    ((this1_t)F_SPTR_SET2)((uintptr_t)&sp, 0, (uintptr_t)m);
    if (!sp) return;
    ((setref_t)F_SETREF)(sp, team, 0x1C);
    *(uint32_t*)(sp + 0x20) = 0;
    o_send(ns, 0, sp);
    obj_release(sp);
}

static void send_team_msg(uintptr_t ns, uintptr_t team) {
    uintptr_t sp = 0;
    void* m = ((create_t)F_CREATE)(TEAMMSG_CLASS);
    if (!m) { logf_("TeamMessage : creation impossible"); return; }
    ((this1_t)F_SPTR_SET2)((uintptr_t)&sp, 0, (uintptr_t)m);
    if (!sp) return;
    ((setref_t)F_SETREF)(sp, team, 0x1C);
    *(uint32_t*)(sp + 0x20) = 0;
    int r = o_send(ns, 0, sp);
    logf_("  TeamMessage IA '%s' joueur='%s' Skill=%d envoye -> %d", sstr(rd32(team + TEAM_NAME)), sstr(rd32(team + TEAM_PLAYER)),
          (int)rd32(team + TEAM_SKILL), r);
    obj_release(sp);
}

// hôte : envoie l'état complet des IA aux invités (removed = équipe retirée, pour annoncer n=0)
static void send_ai_state(const char* why, uintptr_t removed) {
    uintptr_t ns = netservice();
    if (!ns || host_is_local() != 1 || !o_send) return;
    if (pl_count(ns_players()) < 2) { logf_("etat IA (%s) : aucun invite, rien a envoyer", why); return; }
    logf_("Envoi de l'etat IA aux invites (%s) : %d IA", why, g_ai_n);
    for (int k = 0; k < g_ai_n; k++) { set_mark(g_ai[k].team, k, g_ai_n, g_ai[k].src); send_team_msg(ns, g_ai[k].team); }
    if (g_ai_n == 0 && removed) { set_mark(removed, 9, 0, "-"); send_team_msg(ns, removed); }
    g_last_ai_send = GetTickCount();
}

static int __fastcall h_refresh(uintptr_t ns, uintptr_t edx, uintptr_t force);

// ---------------------------------------------------------------- Wormpot en ligne
// Le menu du lobby (MENUTWKXNET.XOM du mod « Wormpot for Multiplayer » de Woitek1993) permet de
// régler les 3 rouleaux, mais chaque PC lit SES valeurs FE.Wormpot.Reel1..3 au lancement
// (WormpotService 0x5D6BC0) : des réglages différents désynchronisent la partie. L'hôte envoie
// donc ses rouleaux aux invités dans un TeamMessage marqué « WUMWP#r1#r2#r3 » (équipe porteuse =
// copie d'une équipe de la caserne) ; l'invité recopie les valeurs.
#define WP_MARK "WUMWP#"
#define F_WP_NAME 0x5D60C0u   // const char* cdecl (reel) : texte traduit (table 0x9205E0, 0..37)
#define WP_MAX_REEL 37u
typedef const char* (__cdecl *wp_name_t)(uint32_t);
static int g_guest_repop;
static uintptr_t g_wp_carrier;
static DWORD g_last_wp_send;
// réglages d'équipe en ligne : définis plus bas
static void send_ts_state(const char* why, int verbose);
static void ts_receive(const char* s);
static void apply_ts_to_game(void);

// lecture comme WormpotService : ressource (0x50B760) puis valeur 32 bits *(*(res+4)+0x1C)
static int read_reels(uint32_t r[3]) {
    static const char* names[3] = {"FE.Wormpot.Reel1", "FE.Wormpot.Reel2", "FE.Wormpot.Reel3"};
    for (int i = 0; i < 3; i++) {
        void* res = NULL; const char* n = names[i];
        if (((res_find_t)F_RES_FIND)(&n, &res) < 0 || !res) {
            static int warned;
            if (!warned) { warned = 1; logf_("Wormpot : ressource %s introuvable", names[i]); }
            return 0;
        }
        uintptr_t inner = rd32((uintptr_t)res + 4);
        r[i] = inner ? rd32(inner + 0x1C) : 0;
        res_release(res);
    }
    return 1;
}

static void send_team_msg_quiet(uintptr_t ns, uintptr_t team);
static void set_uint_res(const char* name, uint32_t v);

// WXFE.Wormpot.NameN (texte affiché dans le lobby) n'est recalculé par le jeu qu'en quittant le menu
// Wormpot (0x7275AE) : après une partie ou une réception, on le recalcule.
static void sync_wp_names(void) {
    static const char* nm[3] = {"WXFE.Wormpot.Name1", "WXFE.Wormpot.Name2", "WXFE.Wormpot.Name3"};
    uint32_t r[3];
    if (!read_reels(r)) return;
    for (int i = 0; i < 3; i++) {
        if (r[i] > WP_MAX_REEL) continue;
        const char* txt = ((wp_name_t)F_WP_NAME)(r[i]);
        if (!txt) continue;
        if (strcmp(get_str(nm[i]), txt) == 0) continue;
        const char* k = nm[i];
        ((set_str_t)F_SET_STRING)(&k, txt);
        logf_("Wormpot affiche : %s = '%s' (rouleau %u)", nm[i], txt, r[i]);
    }
}
static void send_wp_state(const char* why, int verbose) {
    uintptr_t ns = netservice();
    if (!ns || host_is_local() != 1 || !o_send || pl_count(ns_players()) < 2) return;
    uint32_t r[3];
    if (!read_reels(r)) return;
    if (!g_wp_carrier) {
        uintptr_t t[4]; void* res;
        int n = barracks(t, 4, &res);
        if (n > 0) g_wp_carrier = clone_team(t[0]);
        res_release(res);
        if (!g_wp_carrier) return;
    }
    char mark[64];
    snprintf(mark, sizeof mark, WP_MARK "%u#%u#%u", r[0], r[1], r[2]);
    set_xstr(g_wp_carrier + TEAM_PLAYER, mark);
    send_team_msg_quiet(ns, g_wp_carrier);
    g_last_wp_send = GetTickCount();
    static int first = 1;
    if (verbose || first) logf_("Wormpot envoye aux invites (%s) : %u %u %u", why, r[0], r[1], r[2]);
    first = 0;
}

// Poste un message interne de l'interface avec un paramètre texte (comme NetService::RefreshLobby
// le fait pour FE.PopulateTable). Le message garde le pointeur : param doit rester valide.
#define MSG_ALLOC_PTR  0x96D14Cu   // allocateur de messages
#define F_MSG_ALLOC    0x691705u   // thiscall Alloc(taille)
#define F_MSG_ID       0x68BBE6u   // thiscall Registration::GetId(1)
#define F_MSG_INITSTR  0x69151Au   // thiscall StringMsg::Init(id, const char*)
#define F_MSG_POST     0x6910E4u   // cdecl Post(msg)
typedef uintptr_t (__fastcall *this2_t)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
typedef int (__cdecl *post_t)(uintptr_t);
static int g_post_quiet;
static void post_str_msg(uintptr_t reg, const char* param, const char* what) {
    uintptr_t alloc = rd32(MSG_ALLOC_PTR);
    if (!alloc) { logf_("%s : allocateur de messages nul", what); return; }
    uintptr_t m = ((this1_t)F_MSG_ALLOC)(alloc, 0, 0xC);
    if (!m) return;
    uint16_t id = (uint16_t)((this1_t)F_MSG_ID)(reg, 0, 1);
    ((this2_t)F_MSG_INITSTR)(m, 0, id, (uintptr_t)param);
    int r = ((post_t)F_MSG_POST)(m);
    if (!g_post_quiet) logf_("%s$%s poste (id %u) -> %d", what, param, id, r);
}

// La liste des joueurs du lobby (WXNET.GameLobbyList) reçoit ses textes de
// TablePopulationService 0x4C9AA0 (lit WXNET.Lobby.PlayerN.Name, sinon FETXT.WaitingForPlayer),
// exécuté sur FE.PopulateTable$WXPreRound (avec 0x4CAF60, liste d'avant-manche).
#define REG_POPULATETABLE 0x979E78u  // enregistrement de « FE.PopulateTable » (utilisé par RefreshLobby)
static void repopulate_lobby_list(void) {
    post_str_msg(REG_POPULATETABLE, "WXPreRound", "FE.PopulateTable");
}

static int g_force_repop;
static void refresh_lobby(void) {   // via le crochet, pour réécrire aussi les lignes des IA
    uintptr_t ns = netservice();
    g_force_repop = 1;
    if (ns && o_refresh) h_refresh(ns, 0, 1);
}

static int ai_uses(const char* name, int except) {
    for (int k = 0; k < g_ai_n; k++) if (k != except && strcmp(g_ai[k].src, name) == 0) return 1;
    return 0;
}

// remplace l'équipe de l'IA k par une copie de src (la caserne n'est jamais modifiée)
static int ai_set_team(int k, uintptr_t src) {
    uintptr_t c = clone_team(src);
    if (!c) return 0;
    if (g_ai[k].team) obj_release(g_ai[k].team);
    g_ai[k].team = c;
    g_ai[k].level = (int)rd32(c + TEAM_SKILL);
    snprintf(g_ai[k].src, sizeof g_ai[k].src, "%s", sstr(rd32(src + TEAM_NAME)));
    return 1;
}

static void ai_marks(void) {
    for (int k = 0; k < g_ai_n; k++) set_mark(g_ai[k].team, k, g_ai_n, g_ai[k].src);
}

// retire l'IA k (les suivantes remontent d'un emplacement)
static void del_ai_slot(int k, const char* why) {
    if (k < 0 || k >= g_ai_n) return;
    uintptr_t removed = g_ai[k].team;
    logf_("%s : IA %d '%s' retiree", why, k + 1, g_ai[k].src);
    memmove(&g_ai[k], &g_ai[k + 1], (g_ai_n - k - 1) * sizeof(ai_t));
    g_ai_n--;
    memset(&g_ai[g_ai_n], 0, sizeof(ai_t));
    ai_marks();
    send_ai_state(why, removed);
    obj_release(removed);
    refresh_lobby();
}

static void clear_recv(void) {
    for (int k = 0; k < MAX_AI; k++) { if (g_recv[k]) obj_release(g_recv[k]); g_recv[k] = 0; }
    g_recv_n = 0;
}

// ---------------------------------------------------------------- crochets
typedef int (__stdcall *upd_t)(uintptr_t, uintptr_t);
static upd_t o_ns_update;
static com4_t o_tc_addtogame;
#ifndef WUM_NOLOG
static com4_t o_mx_write;
#endif

// 1 si les lignes des IA contiennent les noms attendus (ressources WXNET.Lobby.PlayerN.Name)
static int ai_rows_ok(uintptr_t ns) {
    if (!ns) return 1;
    int host = host_flag(ns) == 1, humans = pl_count(rd32(ns + 0x4A8));
    int na = host ? g_ai_n : g_recv_n;
    for (int k = 0; k < na && humans + k + 1 <= 4; k++) {
        uintptr_t team = host ? g_ai[k].team : g_recv[k];
        const char* src = host ? g_ai[k].src : g_recv_src[k];
        if (!team) continue;
        const char* want = src[0] ? src : sstr(rd32(team + TEAM_NAME));
        char key[64];
        snprintf(key, sizeof key, "WXNET.Lobby.Player%d.Name", humans + k + 1);
        if (strcmp(get_str(key), want) != 0) return 0;
    }
    return 1;
}
static volatile int g_pop_stale;   // le dernier remplissage de la liste a vu des lignes IA fausses

static int __stdcall h_ns_update(uintptr_t ns, uintptr_t dt) {
    int r = o_ns_update(ns, dt);
    static int last_n = -1, last_host = -2;
    static DWORD last_flush;
    if (ns != g_ns_seen) {   // nouvelle session réseau : on oublie les IA de la précédente
        logf_("NetService %08X (nouvelle session)", (unsigned)ns);
        g_ns_seen = ns; clear_recv(); last_n = -1;
    }
    int n = pl_count(rd32(ns + 0x4A8)), host = host_flag(ns);
    if (n != last_n || host != last_host) {
        logf_("NetService : %d joueur(s), hote local=%d, pseudo local='%s', IA preparees=%d", n, host, local_nick(), g_ai_n);
        if (host == 1 && g_ai_n > 0 && n > last_n && last_n >= 0) send_ai_state("nouveau joueur", 0);
        if (host == 1 && n > last_n && last_n >= 0) send_wp_state("nouveau joueur", 1);
        if (host == 1 && n > last_n && last_n >= 0) send_ts_state("nouveau joueur", 1);
        last_n = n; last_host = host;
    }
    if (host == 1 && n >= 2 && GetTickCount() - g_last_wp_send > 3000 && strncmp(get_str("FE.CurrentMenu"), "WXNET.", 6) == 0) {
        static uint32_t last[3] = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
        uint32_t r[3];
        int changed = read_reels(r) && (r[0] != last[0] || r[1] != last[1] || r[2] != last[2]);
        if (changed) memcpy(last, r, sizeof last);
        send_wp_state("periodique", changed);
        send_ts_state("periodique", 0);
    }
    // les messages envoyés pendant l'ouverture du canal d'un nouvel invité sont perdus : dans le lobby,
    // l'hôte renvoie l'état des IA toutes les 3 s (l'invité ne redessine que si l'état change)
    if (host == 1 && g_ai_n > 0 && n >= 2 && GetTickCount() - g_last_ai_send > 3000 &&
        strncmp(get_str("FE.CurrentMenu"), "WXNET.", 6) == 0) {
        static int quiet;
        if (quiet < 5) { quiet++; send_ai_state("periodique", 0); }
        else {   // sans journal après quelques envois
            for (int k = 0; k < g_ai_n; k++) { set_mark(g_ai[k].team, k, g_ai_n, g_ai[k].src); }
            for (int k = 0; k < g_ai_n; k++) send_team_msg_quiet(ns, g_ai[k].team);
            g_last_ai_send = GetTickCount();
        }
    }
    {
        static DWORD last_chk;
        DWORD tc = GetTickCount();
        if (tc - last_chk > 500 && strncmp(get_str("FE.CurrentMenu"), "WXNET.", 6) == 0) {
            last_chk = tc;
            sync_wp_names();
            int bad = !ai_rows_ok(ns), stale = g_pop_stale;
            if (bad || stale) {
                static int cnt;
                if (++cnt <= 20) logf_("Lignes IA du lobby %s (menu '%s') : reecriture", bad ? "effacees par le jeu" : "non affichees",
                                       get_str("FE.CurrentMenu"));
                g_pop_stale = 0;
                g_force_repop = 1;
                g_post_quiet = 1;
                refresh_lobby();
                g_post_quiet = 0;
            }
        }
    }
    if (host != 1) {
        static DWORD t0, last_rp;
        static int step;
        DWORD tn = GetTickCount();
        if (g_guest_repop) { g_guest_repop = 0; t0 = tn; step = 0; }
        static const DWORD delays[3] = {500, 2000, 5000};
        int in_lobby = strncmp(get_str("FE.CurrentMenu"), "WXNET.", 6) == 0;
        if (t0 && step < 3 && tn - t0 >= delays[step]) {
            step++;
            logf_("Invite : nouveau remplissage du lobby (%d/3, menu '%s', IA %d)", step, get_str("FE.CurrentMenu"), g_recv_n);
            if (in_lobby) { refresh_lobby(); repopulate_lobby_list(); }
            last_rp = tn;
        } else if (in_lobby && g_recv_n > 0 && tn - last_rp > 5000) {
            g_post_quiet = 1;
            refresh_lobby(); repopulate_lobby_list();
            g_post_quiet = 0;
            last_rp = tn;
        }
    }
    DWORD now = GetTickCount();
    if (now - last_flush > 300) { log_flush(); last_flush = now; }
    return r;
}

// NetService::Send(msg)
static int __fastcall h_send(uintptr_t ns, uintptr_t edx, uintptr_t msg) {
    int r = o_send(ns, 0, msg);
    if ((uintptr_t)__builtin_return_address(0) == NS_SEND_TEAMS) {
        uintptr_t team = rd32(msg + 0x1C);
        logf_("Envoi equipe joueur au lancement : '%s' joueur='%s'", team ? sstr(rd32(team + TEAM_NAME)) : "-",
              team ? sstr(rd32(team + TEAM_PLAYER)) : "-");
        if (g_ai_n > 0 && GetTickCount() - g_last_ai_send > 3000) send_ai_state("lancement", 0);
        if (GetTickCount() - g_last_wp_send > 1000) send_wp_state("lancement", 1);
        send_ts_state("lancement", 1);
    }
    return r;
}

// TeamMessage::Handle(arg) — this = message
static int __fastcall h_teammsg(uintptr_t msg, uintptr_t edx, uintptr_t arg) {
    uintptr_t team = rd32(msg + 0x1C);
    const char* pl = team ? sstr(rd32(team + TEAM_PLAYER)) : "";
    if (strncmp(pl, WP_MARK, 6) == 0) {
        uint32_t r[3] = {0, 0, 0};
        uint32_t cur[3] = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
        if (host_is_local() != 1 && sscanf(pl + 6, "%u#%u#%u", &r[0], &r[1], &r[2]) == 3 &&
            (!read_reels(cur) || memcmp(r, cur, sizeof r) != 0)) {
            set_uint_res("FE.Wormpot.Reel1", r[0]);
            set_uint_res("FE.Wormpot.Reel2", r[1]);
            set_uint_res("FE.Wormpot.Reel3", r[2]);
            sync_wp_names();
            g_guest_repop = 1;
            uint32_t chk[3] = {0, 0, 0};
            read_reels(chk);
            logf_("Wormpot de l'hote applique : %u %u %u (relu %u %u %u)", r[0], r[1], r[2], chk[0], chk[1], chk[2]);
        }
        return 0;
    }
    if (strncmp(pl, "WUMTS#", 6) == 0) {
        if (host_is_local() != 1) ts_receive(pl + 6);
        return 0;
    }
    if (strncmp(pl, MARK, 4) == 0) {
        int k = -1, n = -1;
        char host[64] = "", src[64] = "";
        sscanf(pl + 4, "%d/%d#%63[^#]#%63[^\n]", &k, &n, host, src);
        static char last_mark[MAX_AI + 1][160];
        int slotlog = (k >= 0 && k < MAX_AI) ? k : MAX_AI;
        int changed = strcmp(last_mark[slotlog], pl) != 0 || (k >= 0 && k < MAX_AI && !g_recv[k]);
        snprintf(last_mark[slotlog], sizeof last_mark[slotlog], "%s", pl);
        if (changed) logf_("TeamMessage IA recu : '%s' Skill=%d marque='%s'", sstr(rd32(team + TEAM_NAME)), (int)rd32(team + TEAM_SKILL), pl);
        if (host_is_local() == 1) return 0;   // nos propres envois
        if (!changed && n == g_recv_n) return 0;   // renvoi périodique identique : rien à faire
        if (n >= 0 && n <= MAX_AI) {
            for (int j = n; j < MAX_AI; j++) { if (g_recv[j]) obj_release(g_recv[j]); g_recv[j] = 0; }
            g_recv_n = n;
        }
        if (k >= 0 && k < n && k < MAX_AI) {
            obj_addref(team);
            if (g_recv[k]) obj_release(g_recv[k]);
            g_recv[k] = team;
            snprintf(g_recv_src[k], sizeof g_recv_src[k], "%s", src);
        }
        snprintf(g_recv_host, sizeof g_recv_host, "%s", host);
        refresh_lobby();
        g_guest_repop = 1;
        return 0;   // pas de joueur correspondant : le jeu l'ignorerait de toute façon
    }
    logf_("TeamMessage recu : '%s' joueur='%s' Skill=%d", team ? sstr(rd32(team + TEAM_NAME)) : "-", pl, team ? (int)rd32(team + TEAM_SKILL) : 0);
    return o_teammsg(msg, 0, arg);
}

static void decorate_lobby(void);
// remplissage de WXNET.GameLobbyList (0x4C9AA0, thiscall sans argument) : lignes occupées reconstruites
// juste avant (decorate_lobby), puis contrôle des lignes des IA
#define F_POP_LOBBYLIST 0x4C9AA0u
static this0_t o_pop_lobby;
static uintptr_t __fastcall h_pop_lobby(uintptr_t self, uintptr_t edx) {
    static int cnt;
    decorate_lobby();
    uintptr_t r = o_pop_lobby(self, 0);
    g_pop_stale = !ai_rows_ok(netservice());
    if (++cnt <= 60) {
        char v[200] = "";
        for (int i = 1; i <= 4; i++) {
            char key[64], b[48];
            snprintf(key, sizeof key, "WXNET.Lobby.Player%d.Name", i);
            snprintf(b, sizeof b, " %d:'%s'", i, get_str(key));
            strncat(v, b, sizeof v - strlen(v) - 1);
        }
        logf_("Liste du lobby remplie (#%d, menu '%s') :%s", cnt, get_str("FE.CurrentMenu"), v);
    }
    return r;
}

// NetService::RefreshLobby(force) : ajoute les lignes des IA après celles des joueurs
static int __fastcall h_refresh(uintptr_t ns, uintptr_t edx, uintptr_t force) {
    int r = o_refresh(ns, 0, force);
    int host = host_flag(ns) == 1;
    int n = host ? g_ai_n : g_recv_n;   // même à 0 : il faut relire et faire remplir la liste (IA retirée)
    int humans = pl_count(rd32(ns + 0x4A8));
    static int last_sig = -1;
    if (humans * 10 + n != last_sig) { logf_("Lobby : %d joueur(s) + %d IA affichees (hote=%d)", humans, n, host); last_sig = humans * 10 + n; }
    for (int k = 0; k < n; k++) {
        int slot = humans + k + 1;
        if (slot > 4) break;
        uintptr_t team = host ? g_ai[k].team : g_recv[k];
        const char* src = host ? g_ai[k].src : g_recv_src[k];
        if (!team) continue;
        char key[64]; const char* kp = key;
        char lvl[32];
        snprintf(lvl, sizeof lvl, "CPU niv %d", (int)rd32(team + TEAM_SKILL));
        snprintf(key, sizeof key, "WXNET.Lobby.Player%d.Name", slot);
        ((set_str_t)F_SET_STRING)(&kp, src[0] ? src : sstr(rd32(team + TEAM_NAME)));
        snprintf(key, sizeof key, "WXNET.Lobby.Player%d.Team", slot);
        ((set_str_t)F_SET_STRING)(&kp, lvl);
        snprintf(key, sizeof key, "WXNET.Team%d.Ready", slot);
        ((set_int_t)F_SET_BOOL)(&kp, 1);
    }
    // lignes libérées par une IA retirée : le RefreshLobby d'origine ne les vide pas tant qu'une
    // fenêtre est ouverte → on les vide nous-mêmes
    static int max_rows;
    if (humans + n > max_rows) max_rows = humans + n;
    for (int slot = humans + n + 1; slot <= max_rows && slot <= 4; slot++) {
        char key[64]; const char* kp = key;
        snprintf(key, sizeof key, "WXNET.Lobby.Player%d.Name", slot);
        ((set_str_t)F_SET_STRING)(&kp, "");
        snprintf(key, sizeof key, "WXNET.Lobby.Player%d.Team", slot);
        ((set_str_t)F_SET_STRING)(&kp, "");
        snprintf(key, sizeof key, "WXNET.Team%d.Ready", slot);
        ((set_int_t)F_SET_BOOL)(&kp, 0);
    }
    max_rows = humans + n;
    // relecture des 4 lignes du lobby (journal seulement si elles changent)
    static char last[512];
    char cur[512] = "";
    for (int i = 1; i <= 4; i++) {
        char key[64], v[120]; const char* kp = key; int rd = -1;
        snprintf(key, sizeof key, "WXNET.Team%d.Ready", i);
        ((get_int_t)F_GET_BOOL)(&kp, &rd);
        snprintf(key, sizeof key, "WXNET.Lobby.Player%d.Name", i);
        const char* nm = get_str(key);
        snprintf(key, sizeof key, "WXNET.Lobby.Player%d.Team", i);
        snprintf(v, sizeof v, " %d:'%s'/'%s'/%d", i, nm, get_str(key), rd & 0xFF);
        strncat(cur, v, sizeof cur - strlen(cur) - 1);
    }
    // la liste affichée (WXNET.GameLobbyList) n'est remplie que sur FE.PopulateTable$WXPreRound :
    // on la redemande si les lignes ont changé, ou toujours après une action du mod (g_force_repop)
    if (strcmp(cur, last) != 0 || g_force_repop) {
        if (strcmp(cur, last) != 0) logf_("Lignes du lobby :%s", cur);
        strcpy(last, cur);
        g_force_repop = 0;
        repopulate_lobby_list();
    }
    return r;
}

static volatile int g_force_add;
static void add_ai_to_game(uintptr_t src, int k, const char* host_nick) {
    if (gameinit_count() >= 4) { logf_("IA %d ignoree : deja 4 equipes", k + 1); return; }
    uintptr_t c = clone_team(src);
    if (!c) { logf_("IA %d : copie impossible", k + 1); return; }
    // ProcessBeginGameMessage est appelé deux fois au lancement et GameInitData est conservé :
    // ne pas ajouter une IA déjà présente (sinon l'hôte aurait une équipe de plus que l'invité)
    char name[64];
    snprintf(name, sizeof name, "%s", sstr(rd32(c + TEAM_NAME)));
    if (gameinit_has(name)) { logf_("IA %d '%s' deja dans la partie", k + 1, name); obj_release(c); return; }
    // même « Player » que l'équipe de l'hôte → même IsLocal que lui sur chaque PC
    uintptr_t hp = find_player(host_nick);
    uintptr_t ht = hp ? rd32(hp + PL_TEAM) : 0;
    const char* owner = ht ? sstr(rd32(ht + TEAM_PLAYER)) : host_nick;
    set_xstr(c + TEAM_PLAYER, owner);
    if (hp) *(uint8_t*)TC_LOCALFLAG = rd8(hp + PL_FLAG);
    uintptr_t tc = rd32(TC_INSTANCE);
    g_force_add = 1;
    int r = tc ? ((com4_t)vfn(tc, 0x20))(tc, c, (uintptr_t)"Barracks", 0x63) : -1;
    g_force_add = 0;
    logf_("IA %d ajoutee a la partie : '%s' joueur='%s' (hote '%s' %s) Skill=%d -> %d", k + 1, sstr(rd32(c + TEAM_NAME)),
          sstr(rd32(c + TEAM_PLAYER)), host_nick, hp ? "trouve" : "INTROUVABLE", (int)rd32(c + TEAM_SKILL), r);
    obj_release(c);
}

// NetService::ProcessBeginGameMessage(msg) — exécuté sur chaque PC
static int __fastcall h_begingame(uintptr_t ns, uintptr_t edx, uintptr_t msg) {
    logf_("ProcessBeginGameMessage (hote local=%d, IA preparees=%d, IA recues=%d)", host_is_local(), g_ai_n, g_recv_n);
    dump_players("BeginGame avant");
    int r = o_begingame(ns, 0, msg);
    dump_players("BeginGame apres");
    if (host_is_local() == 1) {
        const char* me = local_nick();
        for (int k = 0; k < g_ai_n; k++) add_ai_to_game(g_ai[k].team, k, me);
    } else {
        for (int k = 0; k < g_recv_n && k < MAX_AI; k++)
            if (g_recv[k]) add_ai_to_game(g_recv[k], k, g_recv_host);
    }
    apply_ts_to_game();
    dump_gameinit("apres BeginGame");
    log_flush();
    return r;
}

// ---------------------------------------------------------------- choix d'une IA en cliquant sur une ligne du lobby
// Clic sur une ligne → message « Net.Lobby.PlayerSelected$N » (N = 0..3) traité par NetService::HandleMessage.
// Pour l'hôte, sur une ligne libre : on ouvre la fenêtre de liste d'équipes du jeu
// (FE.PopulateTable$WXOnlineTeam puis WXMsg.CreatePopUp$WXFEP.SelectTeam). Sa liste est remplie par
// TablePopulationService 0x4C4110, qui exclut d'habitude les équipes CPU (jne en 0x4C44CE si Skill≠0) :
// pendant ce remplissage on inverse ce saut (je) et l'action finale devient « Net.LocalSelectTeam »,
// que l'on intercepte ensuite : WXFE.NetLocalTeamIndex = index de l'équipe dans la caserne.
#define NS_VT_MSG        0x1Cu       // NetService::HandleMessage(this, msg) stdcall ret 8
#define REG_PLAYERSEL    0x979E60u   // « Net.Lobby.PlayerSelected »
#define REG_LOCALSELTEAM 0x979E58u   // « Net.LocalSelectTeam »
#define REG_CREATEPOPUP  0x979EE0u   // « WXMsg.CreatePopUp »
#define REG_KICKPLAYER   0x979E70u   // « Net.Lobby.KickPlayer » (exclusion, lit WXNet.Lobby.SelectedPlayer)
#define F_POP_ONLINETEAM 0x4C4110u   // TablePopulationService::PopulateOnlineTeams(action) thiscall ret 4
#define POP_SKILL_JUMP   0x4C44CEu   // 0F 85 = jne (exclut Skill≠0) ; 0F 84 = je (garde seulement Skill≠0)
#define F_MSG_ISID       0x68BCCEu   // cdecl IsMessage(id, enregistrement)

typedef int (__cdecl *ismsg_t)(uint32_t, uintptr_t);
static upd_t o_ns_msg;
static thiscall1_t o_pop_teams;
static int g_pick_mode;            // 0 = rien, 1 = fenêtre IA demandée, 2 = liste IA affichée
static int g_pick_slot;
static int g_pick_saved_index = -1;
static DWORD g_pick_time;

static int msg_is(uintptr_t msg, uintptr_t reg) {
    return ((ismsg_t)F_MSG_ISID)(*(uint16_t*)(msg + 4), reg) != 0;
}

static int get_int(const char* name, int def) {
    const char* n = name; int v = def;
    if (((get_int_t)F_GET_BOOL)(&n, &v) < 0) return def;   // 0x50B790, comme ProcessLocalSelectTeam
    return v;
}

static void set_uint_res(const char* name, uint32_t v) {
    void* res = NULL; const char* n = name;
    if (((res_find_t)F_RES_FIND)(&n, &res) < 0 || !res) return;
    ((this1_t)0x69E100u)((uintptr_t)res, 0, (uintptr_t)&v);   // écriture générique + notification
    res_release(res);
}

static void patch_skill_jump(int only_cpu) {
    uint8_t b = only_cpu ? 0x84 : 0x85;
    DWORD old;
    VirtualProtect((void*)(POP_SKILL_JUMP + 1), 1, PAGE_EXECUTE_READWRITE, &old);
    *(uint8_t*)(POP_SKILL_JUMP + 1) = b;
    VirtualProtect((void*)(POP_SKILL_JUMP + 1), 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)POP_SKILL_JUMP, 6);
}

// CrcCheckService (0x635570) : boucle sur la table 0x922508 (89 fichiers .lub / XOM de langue / Tweak) ;
// au premier CRC faux, jne 0x635667 mémorise l'index puis Update() appelle XomGetApp()->Quit()
// → crash R6025 « pure virtual function call ». 75 4D → EB 00 : la comparaison passe toujours
// (même patch que WUM.Loader.asi), pour les fichiers du jeu modifiés (MENUTWKXNET, LOCAL, FreFE…).
#define CRC_CHECK_JNE    0x635618u
static void patch_crc_check(void) {
    uint8_t* p = (uint8_t*)CRC_CHECK_JNE;
    if (p[0] == 0xEB && p[1] == 0x00) { logf_("controle CRC deja neutralise"); return; }
    if (p[0] != 0x75 || p[1] != 0x4D) { logf_("controle CRC : octets inattendus %02X %02X, non patche", p[0], p[1]); return; }
    DWORD old;
    VirtualProtect(p, 2, PAGE_EXECUTE_READWRITE, &old);
    p[0] = 0xEB; p[1] = 0x00;
    VirtualProtect(p, 2, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 2);
    logf_("controle CRC neutralise (0x%08X)", CRC_CHECK_JNE);
}

// Icône de niveau, comme la liste d'équipes locale (TablePopulationService 0x4C0213) : colonne d'icône
// (classe 0x87AF94 : IconName +0x14, ScaleX +0x18, ScaleY +0x1C, WidthAdjust +0x20, AutoScale +0x34)
// précédée d'une colonne d'espace (0x87B044). On l'ajoute à chaque ligne juste avant que 0x4C4110 ne
// range la ligne dans la liste (appel de 0x6C36BD en 0x4C4683).
#define F_ADD_CHILD     0x6C36BDu   // cdecl AddChild(parent, champ, pas, enfant)
#define RET_ADD_ROW     0x4C4688u   // retour de l'ajout d'une ligne dans 0x4C4110
#define CLASS_ICONCOL   0x87AF94u
#define CLASS_GAPCOL    0x87B044u
typedef int (__cdecl *addchild_t)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
static addchild_t o_add_child;
static volatile int g_pop_ai;

static const char* skill_icon(int skill) {
    static const char* icons[6] = {"WXFE.TeamInfo03:3", "WXFE.TeamInfo02:2", "WXFE.TeamInfo02:3",
                                   "WXFE.TeamInfo03:0", "WXFE.TeamInfo03:1", "WXFE.TeamInfo03:2"};
    return icons[(skill >= 0 && skill <= 5) ? skill : 0];
}

static void add_skill_icon(uintptr_t row) {
    uint32_t m = 0;
    uintptr_t cols = (uintptr_t)((void* (__cdecl *)(uintptr_t, uintptr_t, uint32_t*))0x43CF50u)(row, 0x64, &m);
    int skill = -1;
    char name[64] = "";
    uintptr_t t[64]; void* res;
    int n = barracks(t, 64, &res);
    for (uint32_t c = 0; c < m && c < 8 && skill < 0; c++) {
        uintptr_t col = rd32(cols + 4 * c);
        const char* cs = sstr(rd32(col + 0x14));
        for (int i = 0; i < n; i++)
            if (strcmp(cs, sstr(rd32(t[i] + TEAM_NAME))) == 0) { skill = (int)rd32(t[i] + TEAM_SKILL); snprintf(name, sizeof name, "%s", cs); break; }
    }
    res_release(res);
    if (skill <= 0) { logf_("icone : equipe de la ligne %08X non trouvee", (unsigned)row); return; }
    uintptr_t gap = (uintptr_t)((create_t)F_CREATE)(CLASS_GAPCOL);
    uintptr_t ico = (uintptr_t)((create_t)F_CREATE)(CLASS_ICONCOL);
    if (!gap || !ico) return;
    obj_addref(gap); obj_addref(ico);
    set_xstr(ico + 0x14, skill_icon(skill));
    *(float*)(ico + 0x18) = 15.0f;
    *(float*)(ico + 0x1C) = 15.0f;
    *(float*)(ico + 0x20) = 0.0f;
    *(uint8_t*)(ico + 0x34) = 0;
    o_add_child(row, 0x64, 0x14, gap);
    o_add_child(row, 0x64, 0x14, ico);
    obj_release(gap); obj_release(ico);
    logf_("icone %s ajoutee pour '%s' (Skill %d)", skill_icon(skill), name, skill);
}

static int __cdecl h_add_child(uintptr_t parent, uintptr_t field, uintptr_t stride, uintptr_t child) {
    if (g_pop_ai && field == 0x14 && (uintptr_t)__builtin_return_address(0) == RET_ADD_ROW) add_skill_icon(child);
    return o_add_child(parent, field, stride, child);
}

static int __fastcall h_pop_teams(uintptr_t svc, uintptr_t edx, uintptr_t action) {
    if (g_pick_mode == 1 && GetTickCount() - g_pick_time < 10000) {
        logf_("Liste d'equipes : mode IA (equipes CPU seulement), emplacement IA %d", g_pick_slot + 1);
        patch_skill_jump(1);
        g_pop_ai = o_add_child != NULL;
        int r = o_pop_teams(svc, 0, (uintptr_t)"Net.LocalSelectTeam");
        g_pop_ai = 0;
        patch_skill_jump(0);
        g_pick_mode = 2;
        return r;
    }
    if (g_pick_mode) { logf_("Liste d'equipes normale : choix IA abandonne"); g_pick_mode = 0; }
    return o_pop_teams(svc, 0, action);
}

static void open_ai_picker(int slot) {
    g_pick_slot = slot;
    g_pick_mode = 1;
    g_pick_time = GetTickCount();
    g_pick_saved_index = get_int("WXFE.NetLocalTeamIndex", -1);
    logf_("Ouverture du choix d'equipe IA pour l'emplacement %d (index local sauve %d)", slot + 1, g_pick_saved_index);
    post_str_msg(REG_POPULATETABLE, "WXOnlineTeam", "FE.PopulateTable");
    post_str_msg(REG_CREATEPOPUP, "WXFEP.SelectTeam", "WXMsg.CreatePopUp");
}

static void apply_ai_pick(void) {
    int idx = get_int("WXFE.NetLocalTeamIndex", -1);
    if (g_pick_saved_index >= 0) set_uint_res("WXFE.NetLocalTeamIndex", (uint32_t)g_pick_saved_index);
    uintptr_t t[64]; void* res;
    int n = barracks(t, 64, &res);
    int slot = g_pick_slot;
    logf_("Equipe IA choisie : index %d / %d, emplacement %d", idx, n, slot + 1);
    if (idx < 0 || idx >= n || (int)rd32(t[idx] + TEAM_SKILL) <= 0) { logf_("choix invalide"); res_release(res); return; }
    const char* nm = sstr(rd32(t[idx] + TEAM_NAME));
    if (ai_uses(nm, slot < g_ai_n ? slot : -1)) { logf_("'%s' est deja une IA de la partie", nm); res_release(res); return; }
    int humans = pl_count(ns_players());
    if (slot > g_ai_n) slot = g_ai_n;
    if (slot == g_ai_n && (g_ai_n >= MAX_AI || humans + g_ai_n >= 4)) { logf_("4 equipes maximum"); res_release(res); return; }
    int ok = ai_set_team(slot, t[idx]);
    res_release(res);
    if (!ok) { return; }
    if (slot == g_ai_n) g_ai_n++;
    ai_marks();
    logf_("IA %d = '%s' Skill=%d (choisie dans la liste)", slot + 1, g_ai[slot].src, g_ai[slot].level);
    send_ai_state("choix", 0);
    refresh_lobby();
}

// ---------------------------------------------------------------- réglages d'équipe en ligne
// En local, un clic sur une équipe ouvre WXFEP.TeamSettings (MENUTWKX.XOM) : nombre de vers, couleur
// d'alliés, handicap, « Enlever une équipe », « Accepter ». La fenêtre lit/écrit WXFE.TeamSelectedName /
// Worms / AlliedGroup / Handicap ; ses flèches font WXMsg.SetDataResource puis FE.PopulateTable$WXTeamSettings
// (TablePopulationService 0x4C0640) ; « Accepter » = WXMsg.PrepareTeam$Update, « Enlever » =
// WXMsg.PrepareTeam$RemoveTeam (TeamControlService 0x742AB0 → setters GameInitData 0x67C882 vers,
// 0x67C7E1 handicap, 0x67C92D alliés). La version en ligne (WXNETP.TeamSettings, GameBrowser/GameSpy)
// n'existe pas dans les menus PC.
// Ici : l'hôte clique une ligne OCCUPÉE (humain ou IA) → on remplit les ressources, on ouvre la fenêtre
// locale, on intercepte PrepareTeam$Update/RemoveTeam ; les réglages sont envoyés aux invités
// (TeamMessage « WUMTS#clé,vers,handicap,alliés|… ») et appliqués dans GameInitData sur chaque PC après
// ProcessBeginGameMessage (lib_SetupMultiplayerTeam lit T*_NumOfWorms / T*_AlliedGroup / T*_Handicap).
// Clé : pseudo Steam du joueur (« ST:… ») pour un humain, « IA:<équipe> » pour une IA.
#define F_TC_PREPARETEAM 0x742AB0u   // thiscall (this, const char* param) ret 4
#define POP_TS_NETJUMP   0x4C08A7u   // je → WXNETP.TeamSettingsList (absente sur PC) : neutralisé
#define F_POP_TEAMSET    0x4C0640u   // TablePopulationService : remplit WXFEP.TeamSettingsList
#define MAX_TS 8
#define TS_MAX_WORMS_TOTAL 16
typedef struct { char key[64]; int worms, hand, ally; } ts_t;
static ts_t g_ts[MAX_TS];
static int g_ts_n;
static int g_ts_open, g_ts_row, g_ts_ai = -1;
static char g_ts_key[64];
static DWORD g_ts_time, g_last_ts_send;
static uintptr_t g_ts_carrier;
static thiscall1_t o_prepare;
static this0_t o_pop_teamset;

static ts_t* ts_find(const char* key, int create) {
    for (int i = 0; i < g_ts_n; i++) if (strcmp(g_ts[i].key, key) == 0) return &g_ts[i];
    if (!create || g_ts_n >= MAX_TS || !key[0]) return NULL;
    ts_t* t = &g_ts[g_ts_n++];
    snprintf(t->key, sizeof t->key, "%s", key);
    t->worms = 4; t->hand = 0; t->ally = 0;
    return t;
}

static int read_res_u32(const char* name, uint32_t* v) {
    void* res = NULL; const char* n = name;
    if (((res_find_t)F_RES_FIND)(&n, &res) < 0 || !res) return 0;
    uintptr_t inner = rd32((uintptr_t)res + 4);
    *v = inner ? rd32(inner + 0x1C) : 0;
    res_release(res);
    return 1;
}

static void send_ts_state(const char* why, int verbose) {
    uintptr_t ns = netservice();
    if (!ns || host_is_local() != 1 || !o_send || pl_count(ns_players()) < 2 || g_ts_n == 0) return;
    if (!g_ts_carrier) {
        uintptr_t t[4]; void* res;
        int n = barracks(t, 4, &res);
        if (n > 0) g_ts_carrier = clone_team(t[0]);
        res_release(res);
        if (!g_ts_carrier) return;
    }
    char mark[640] = "WUMTS#";
    for (int i = 0; i < g_ts_n; i++) {
        char e[96];
        snprintf(e, sizeof e, "%s%s,%d,%d,%d", i ? "|" : "", g_ts[i].key, g_ts[i].worms, g_ts[i].hand, g_ts[i].ally);
        strncat(mark, e, sizeof mark - strlen(mark) - 1);
    }
    set_xstr(g_ts_carrier + TEAM_PLAYER, mark);
    send_team_msg_quiet(ns, g_ts_carrier);
    g_last_ts_send = GetTickCount();
    if (verbose) logf_("Reglages d'equipe envoyes aux invites (%s) : %s", why, mark + 6);
}

static void ts_receive(const char* str) {
    static char last[640];
    if (strcmp(last, str) == 0) return;
    snprintf(last, sizeof last, "%s", str);
    g_ts_n = 0;
    char buf[640]; snprintf(buf, sizeof buf, "%s", str);
    for (char* e = strtok(buf, "|"); e && g_ts_n < MAX_TS; e = strtok(NULL, "|")) {
        char key[64]; int w, h, a;
        char* c = strchr(e, ',');
        if (!c) continue;
        *c = 0; snprintf(key, sizeof key, "%s", e);
        if (sscanf(c + 1, "%d,%d,%d", &w, &h, &a) != 3) continue;
        ts_t* t = ts_find(key, 1);
        if (t) { t->worms = w; t->hand = h; t->ally = a; }
    }
    logf_("Reglages d'equipe de l'hote recus : %s (%d equipe(s))", str, g_ts_n);
    g_guest_repop = 1;
}

// sur chaque PC, après l'ajout des équipes (humaines par le jeu, IA par le mod) : mêmes données → même résultat
static void apply_ts_to_game(void) {
    if (g_ts_n == 0) return;
    void* res;
    uintptr_t gi = res_container("GM.GameInitData", &res);
    if (!gi) { res_release(res); return; }
    int n = (int)rd32(gi + GI_NUMTEAMS);
    if (n > 4) n = 4;
    int worms[4];
    for (int k = 0; k < n; k++) {
        uintptr_t t = gi_team(gi, k);
        char key[80];
        if ((int)rd32(t + 0x24) > 0) snprintf(key, sizeof key, "IA:%s", sstr(rd32(t)));
        else snprintf(key, sizeof key, "%s", sstr(rd32(t + 4)));
        worms[k] = (int)((this1_t)0x67C836u)(gi, 0, k);
        ts_t* s = ts_find(key, 0);
        if (!s) { logf_("Reglages T%d '%s' : aucun (valeurs du jeu)", k + 1, key); continue; }
        int w = s->worms < 1 ? 1 : s->worms > 6 ? 6 : s->worms;
        int h = s->hand < -1 ? -1 : s->hand > 1 ? 1 : s->hand;
        int a = s->ally < 0 ? 0 : s->ally > 3 ? 3 : s->ally;
        ((this2_t)0x67C882u)(gi, 0, (uintptr_t)k, (uintptr_t)w);
        ((this2_t)0x67C7E1u)(gi, 0, (uintptr_t)k, (uintptr_t)h);
        ((this2_t)0x67C92Du)(gi, 0, (uintptr_t)k, (uintptr_t)a);
        worms[k] = w;
        logf_("Reglages T%d '%s' appliques : vers=%d handicap=%d allies=%d", k + 1, key, w, h, a);
    }
    // le jeu limite le total à 16 vers (0x73620F) : on retire des vers en partant de la dernière équipe
    int total = 0;
    for (int k = 0; k < n; k++) total += worms[k];
    for (int k = n - 1; total > TS_MAX_WORMS_TOTAL && k >= 0; k--)
        while (total > TS_MAX_WORMS_TOTAL && worms[k] > 1) { worms[k]--; total--; }
    for (int k = 0; k < n; k++)
        if (worms[k] != (int)((this1_t)0x67C836u)(gi, 0, k)) {
            ((this2_t)0x67C882u)(gi, 0, (uintptr_t)k, (uintptr_t)worms[k]);
            logf_("T%d : vers ramenes a %d (16 vers maximum en tout)", k + 1, worms[k]);
        }
    res_release(res);
}

// ligne r du lobby : occupée ? vers et groupe d'alliés (réglages de l'hôte, sinon défauts du jeu : 4, r)
static void ts_row_info(uintptr_t ns, int row, int* occupied, int* worms, int* ally) {
    int humans = pl_count(rd32(ns + 0x4A8)), host = host_flag(ns) == 1;
    int na = host ? g_ai_n : g_recv_n;
    char key[80] = "";
    *occupied = 0; *worms = 4; *ally = row & 3;
    if (row < humans) {
        uintptr_t pp = pl_get(rd32(ns + 0x4A8), row);
        if (pp) snprintf(key, sizeof key, "%s", sstr(rd32(pp + PL_NICK)));
        *occupied = 1;
    } else if (row < humans + na) {
        const char* src = host ? g_ai[row - humans].src : g_recv_src[row - humans];
        snprintf(key, sizeof key, "IA:%s", src);
        *occupied = 1;
    }
    ts_t* t = key[0] ? ts_find(key, 0) : NULL;
    if (t) { *worms = t->worms; *ally = t->ally & 3; }
}

static void ts_row_extra(uintptr_t ns, int row, int* hcap, int* skill) {
    int humans = pl_count(rd32(ns + 0x4A8)), host = host_flag(ns) == 1;
    int na = host ? g_ai_n : g_recv_n;
    char key[80] = "";
    *hcap = 0; *skill = 0;
    if (row < humans) {
        uintptr_t pp = pl_get(rd32(ns + 0x4A8), row);
        if (pp) snprintf(key, sizeof key, "%s", sstr(rd32(pp + PL_NICK)));
    } else if (row < humans + na) {
        uintptr_t team = host ? g_ai[row - humans].team : g_recv[row - humans];
        const char* src = host ? g_ai[row - humans].src : g_recv_src[row - humans];
        if (team) *skill = (int)rd32(team + TEAM_SKILL);
        snprintf(key, sizeof key, "IA:%s", src);
    }
    ts_t* t = key[0] ? ts_find(key, 0) : NULL;
    if (t) *hcap = t->hand;
}

static void open_team_settings(uintptr_t ns, int row) {
    int humans = pl_count(rd32(ns + 0x4A8));
    char name[96] = "";
    g_ts_ai = -1;
    if (row < humans) {
        uintptr_t pp = pl_get(rd32(ns + 0x4A8), row);
        uintptr_t team = pp ? rd32(pp + PL_TEAM) : 0;
        snprintf(g_ts_key, sizeof g_ts_key, "%s", pp ? sstr(rd32(pp + PL_NICK)) : "");
        if (team) snprintf(name, sizeof name, "%s", sstr(rd32(team + TEAM_NAME)));
        if (!name[0]) { char k[64]; snprintf(k, sizeof k, "WXNET.Lobby.Player%d.Name", row + 1); snprintf(name, sizeof name, "%s", get_str(k)); }
    } else {
        g_ts_ai = row - humans;
        snprintf(name, sizeof name, "%s", g_ai[g_ts_ai].src);
        snprintf(g_ts_key, sizeof g_ts_key, "IA:%s", g_ai[g_ts_ai].src);
    }
    ts_t* t = ts_find(g_ts_key, 0);
    int w = t ? t->worms : 4, h = t ? t->hand : 0, a = t ? t->ally : (row & 3);
    const char* kn = "WXFE.TeamSelectedName";
    ((set_str_t)F_SET_STRING)(&kn, name);
    set_uint_res("WXFE.TeamSelectedWorms", (uint32_t)w);
    set_uint_res("WXFE.TeamSelectedHandicap", (uint32_t)h);
    set_uint_res("WXFE.TeamSelectedAlliedGroup", (uint32_t)a);
    g_ts_open = 1; g_ts_row = row; g_ts_time = GetTickCount();
    const char* sub = get_str("GameOver.SubGameType");
    const char* pop = (!strcmp(sub, "Statue") || !strcmp(sub, "Destruction")) ? "WXFEP.TeamSettingsNoHandicap"
                    : !strcmp(sub, "Survivor") ? "WXFEP.TeamSettingsNoAlly" : "WXFEP.TeamSettings";
    logf_("Reglages d'equipe : ligne %d '%s' cle '%s' (vers=%d handicap=%d allies=%d, mode '%s') -> %s", row + 1, name,
          g_ts_key, w, h, a, sub, pop);
    post_str_msg(REG_POPULATETABLE, "WXTeamSettings", "FE.PopulateTable");
    post_str_msg(REG_CREATEPOPUP, pop, "WXMsg.CreatePopUp");
}

// pendant le remplissage de la fenêtre : GameInitData contient encore les équipes de la partie
// précédente, que le jeu compterait dans la limite de 16 vers → on masque leur nombre (0)
static uintptr_t __fastcall h_pop_teamset(uintptr_t svc, uintptr_t edx) {
    if (!g_ts_open) return o_pop_teamset(svc, 0);
    void* res;
    uintptr_t gi = res_container("GM.GameInitData", &res);
    uint32_t saved = gi ? rd32(gi + GI_NUMTEAMS) : 0;
    if (gi) *(uint32_t*)(gi + GI_NUMTEAMS) = 0;
    uintptr_t r = o_pop_teamset(svc, 0);
    if (gi) *(uint32_t*)(gi + GI_NUMTEAMS) = saved;
    res_release(res);
    return r;
}

static int __fastcall h_prepare(uintptr_t tc, uintptr_t edx, uintptr_t param) {
    const char* prm = param ? (const char*)param : "";
    if (g_ts_open && (GetTickCount() - g_ts_time > 300000 || strncmp(get_str("FE.CurrentMenu"), "WXNET.", 6) != 0)) g_ts_open = 0;
    if (!g_ts_open) return o_prepare(tc, 0, param);
    logf_("PrepareTeam$%s (fenetre d'equipe en ligne, cle '%s')", prm, g_ts_key);
    if (strcmp(prm, "Update") == 0) {
        uint32_t w = 4, h = 0, a = 0;
        read_res_u32("WXFE.TeamSelectedWorms", &w);
        read_res_u32("WXFE.TeamSelectedHandicap", &h);
        read_res_u32("WXFE.TeamSelectedAlliedGroup", &a);
        ts_t* t = ts_find(g_ts_key, 1);
        if (t) { t->worms = (int)w; t->hand = (int)h; t->ally = (int)a; }
        logf_("Reglages acceptes pour '%s' : vers=%d handicap=%d allies=%d", g_ts_key, (int)w, (int)h, (int)a);
        g_ts_open = 0;
        send_ts_state("choix", 1);
        g_force_repop = 1;
        refresh_lobby();
    } else if (strcmp(prm, "RemoveTeam") == 0) {
        g_ts_open = 0;
        if (g_ts_ai >= 0 && g_ts_ai < g_ai_n) del_ai_slot(g_ts_ai, "Enlever");
        else if (g_ts_row > 0) {
            set_uint_res("WXNet.Lobby.SelectedPlayer", (uint32_t)g_ts_row);
            logf_("Enlever : exclusion du joueur de la ligne %d", g_ts_row + 1);
            post_str_msg(REG_KICKPLAYER, "", "Net.Lobby.KickPlayer");
        } else logf_("Enlever : c'est l'hote, ignore");
    }
    return 0;   // les autres actions (Select…, CancelEdit → FE.PreviousMenu) quitteraient le lobby
}

static const char* worms_icon(int w) {
    static const char* ic[6] = {"WXFE.TeamInfo01:0", "WXFE.TeamInfo01:1", "WXFE.TeamInfo01:2",
                                "WXFE.TeamInfo01:3", "WXFE.TeamInfo02:0", "WXFE.TeamInfo02:1"};
    return ic[(w >= 1 && w <= 6) ? w - 1 : 3];
}

// Lobby affiché comme la liste locale (TablePopulationService 0x4BE910) : les lignes OCCUPÉES sont
// reconstruites à chaque remplissage puis remises dans la liste (le widget ne dessine que les colonnes
// présentes à la création d'une ligne) ; les lignes vides restent celles du menu (MENUTWKXNET.XOM).
// Valeurs reprises de 0x4BEF1F…0x4BF2BD :
//   ligne 0x87AE8C : +0x20 = 6, +0x24 = 9, +0x28 = 7 (bordures), +0x2C = 26, +0x48 = 42.0 (hauteur),
//     +0x68 = 1 ; messages de sélection : 0x4BD320(ligne, n, 2, &ancien) → n XString à remplir (+0x34)
//   [handicap 8×8 si ≠ 0], texte (+0x14 texte, +0x1C 20.0, +0x20 190 − 16 si handicap, +0x24 = 4,
//     +0x28 = alliés + 15 = couleur du nom), contour BlackSplodge1 34×17 −60, tête de ver (icône de
//     niveau, rose pour un humain) 17×17 −19, « ×N » 17×17 −40 (InvisibleToBorder)
//   texte : table de chaînes 0x87AF10 (comme le menu du lobby) avec la clé WXNET.Lobby.PlayerN.Name,
//     dont elle affiche la valeur
//   liste : 0x6C37D5(ctr, 0x14, 0, 0, n) vide, AddChild(ctr, 0x14, 0, ligne) ajoute.
#define DESC_LISTROW  0x87AE8Cu
#define DESC_TEXTCOL  0x87AF10u   // WXFE_StringTableColumns
static uintptr_t g_xom_rows[8], g_xom_ctr;   // lignes d'origine du menu (référence gardée)
static int g_xom_n;

static uintptr_t mk_obj(uintptr_t desc) {
    uintptr_t o = (uintptr_t)((create_t)F_CREATE)(desc);
    obj_addref(o);
    return o;
}
static uintptr_t mk_icon(const char* name, float sx, float sy, float adj, int inv) {
    uintptr_t c = mk_obj(CLASS_ICONCOL);
    if (!c) return 0;
    set_xstr(c + 0x14, name);
    *(float*)(c + 0x18) = sx; *(float*)(c + 0x1C) = sy; *(float*)(c + 0x20) = adj;
    *(uint8_t*)(c + 0x34) = 0; *(uint8_t*)(c + 0x35) = (uint8_t)inv;
    return c;
}
static void row_add(uintptr_t row, uintptr_t child) {
    if (!child) return;
    o_add_child(row, 0x64, 0x14, child);
    obj_release(child);
}
static uintptr_t build_lobby_row(int i, const char* name, int w, int a, int hcap, int skill) {
    uintptr_t row = mk_obj(DESC_LISTROW);
    if (!row) return 0;
    *(uint32_t*)(row + 0x20) = 6; *(uint32_t*)(row + 0x24) = 9; *(uint32_t*)(row + 0x28) = 7;
    *(uint32_t*)(row + 0x2C) = 0x1A; *(float*)(row + 0x48) = 42.0f; *(uint8_t*)(row + 0x68) = 1;
    uintptr_t prev = 0;
    uintptr_t msgs = ((uintptr_t (__fastcall*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t))0x4BD320u)(row, 0, 2, 2, (uintptr_t)&prev);
    if (msgs) {
        char m0[48], m1[80];
        snprintf(m0, sizeof m0, "Net.Lobby.PlayerSelected$%d", i);
        snprintf(m1, sizeof m1, "WXMsg.SetDataResource^WXNet.Lobby.SelectedPlayer^%d", i);
        set_xstr(msgs, m0);
        set_xstr(msgs + 4, m1);
    }
    static const char* hic[3] = {"WXFE.HandicapAllies:1", "WXFE.HandicapAllies:2", "WXFE.HandicapAllies:0"};
    if (hcap) row_add(row, mk_icon(hic[(hcap < 0 ? -1 : 1) + 1], 8.0f, 8.0f, 0.0f, 0));
    uintptr_t txt = mk_obj(DESC_TEXTCOL);
    if (txt) {
        char key[48];
        snprintf(key, sizeof key, "WXNET.Lobby.Player%d.Name", i + 1);
        set_xstr(txt + 0x14, key);
        *(float*)(txt + 0x1C) = 20.0f;
        *(float*)(txt + 0x20) = hcap ? 174.0f : 190.0f;
        *(uint32_t*)(txt + 0x24) = 4; *(uint32_t*)(txt + 0x28) = (uint32_t)((a & 3) + 0xF);
        *(uint32_t*)(txt + 0x2C) = 3; *(uint32_t*)(txt + 0x30) = 19;    // couleurs du XOM du lobby
        *(uint32_t*)(txt + 0x34) = 2; *(uint32_t*)(txt + 0x38) = 3; *(uint32_t*)(txt + 0x3C) = 2;
        *(uint8_t*)(txt + 0x48) = 0;
        row_add(row, txt);
    }
    row_add(row, mk_icon("WXFE.BlackSplodge1", 34.0f, 17.0f, -60.0f, 1));
    row_add(row, mk_icon(skill_icon(skill), 17.0f, 17.0f, -19.0f, 1));
    row_add(row, mk_icon(worms_icon(w), 17.0f, 17.0f, -40.0f, 1));
    return row;
}

static void decorate_lobby(void) {
    uintptr_t ns = netservice();
    if (!o_add_child) return;
    void* res = NULL; const char* nm = "WXNET.GameLobbyList";
    if (((res_find_t)F_RES_FIND)(&nm, &res) < 0 || !res) return;
    uint32_t tmp[16]; memset(tmp, 0, sizeof tmp); tmp[1] = 0xE3A8;
    uintptr_t obj = ((this1_t)0x69DEE0u)((uintptr_t)res, 0, (uintptr_t)tmp);
    obj_addref(obj);
    uintptr_t ctr = obj ? (uintptr_t)((create_t)0x4BD050u)(obj) : 0;
    obj_addref(ctr);
    obj_release(obj);
    static int logged;
    if (ctr && rdok((void*)(ctr + 0x14), 4) && rd32(ctr + 0x14)) {
        uintptr_t arr = rd32(ctr + 0x14);
        int n = (int)rd32(arr + 0x18);
        if (ctr != g_xom_ctr) {   // nouvelle liste (chargement du XOM) : on garde ses lignes d'origine
            for (int i = 0; i < g_xom_n; i++) obj_release(g_xom_rows[i]);
            g_xom_n = 0;
            for (int i = 0; i < n && i < 8; i++) { g_xom_rows[i] = rd32(arr + 0x20 + 4 * i); obj_addref(g_xom_rows[i]); g_xom_n++; }
            g_xom_ctr = ctr;
            logf_("Lobby : %d lignes d'origine gardees (liste %08X)", g_xom_n, (unsigned)ctr);
        }
        if (ns && g_xom_n > 0) {
            uintptr_t rows[8];
            for (int i = 0; i < g_xom_n; i++) {
                int occ = 0, w = 4, a = 0, hcap = 0, skill = 0;
                if (i < 4) ts_row_info(ns, i, &occ, &w, &a);
                if (occ) ts_row_extra(ns, i, &hcap, &skill);
                uintptr_t r = 0;
                if (occ) {
                    char key[48];
                    snprintf(key, sizeof key, "WXNET.Lobby.Player%d.Name", i + 1);
                    r = build_lobby_row(i, get_str(key), w, a, hcap, skill);
                    if (logged < 8) { logged++; logf_("Lobby ligne %d reconstruite : '%s' vers=%d allies=%d handicap=%d niveau=%d", i + 1, get_str(key), w, a, hcap, skill); }
                }
                if (!r) { r = g_xom_rows[i]; obj_addref(r); }
                rows[i] = r;
            }
            ((void (__cdecl*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t))0x6C37D5u)(ctr, 0x14, 0, 0, rd32(rd32(ctr + 0x14) + 0x18));
            for (int i = 0; i < g_xom_n; i++) { o_add_child(ctr, 0x14, 0, rows[i]); obj_release(rows[i]); }
        }
    }
    obj_release(ctr);
    ((this0_t)0x6A22A0u)((uintptr_t)tmp, 0);
    res_release(res);
}

static int __stdcall h_ns_msg(uintptr_t ns, uintptr_t msg) {
    if (msg_is(msg, REG_PLAYERSEL)) {
        const char* prm = sstr(rd32(msg + 8));
        int row = prm[0] - '0', humans = pl_count(rd32(ns + 0x4A8)), host = host_flag(ns) == 1;
        logf_("Clic ligne du lobby %d (joueurs %d, IA %d, hote %d)", row + 1, humans, g_ai_n, host);
        if (host && row >= 0 && row < humans + g_ai_n && row <= 3 && o_prepare) {
            // ligne occupée (humain ou IA) : fenêtre de réglages d'équipe (vers, alliés, handicap, enlever)
            open_team_settings(ns, row);
            return 0;
        }
        if (host && row >= humans && row <= 3) {
            int slot = row - humans;
            if (slot > g_ai_n) slot = g_ai_n;
            if (slot == g_ai_n && (g_ai_n >= MAX_AI || humans + g_ai_n >= 4)) { return 0; }
            open_ai_picker(slot);
            return 0;
        }
    } else if (msg_is(msg, REG_LOCALSELTEAM) && g_pick_mode == 2) {
        g_pick_mode = 0;
        apply_ai_pick();
        return 0;
    }
    return o_ns_msg(ns, msg);
}

// TeamControlService 0x7388C9(joueur) : vrai si une équipe de GameInitData a déjà ce Player
// (AddTeamToGame refuse alors l'équipe). L'IA garde le Player de l'hôte (c'est lui qui joue ses
// tours) : on lève ce refus uniquement pendant l'ajout des IA.
#define F_TC_PLAYERUSED 0x7388C9u
static thiscall1_t o_tc_playerused;
static int __fastcall h_tc_playerused(uintptr_t tc, uintptr_t edx, uintptr_t player) {
    if (g_force_add) return 0;
    return o_tc_playerused(tc, 0, player);
}

// ---------------------------------------------------------------- déterminisme de l'IA en ligne
// L'IA est calculée sur chaque PC (elle consomme le générateur aléatoire logique, comparé par la
// validation réseau). Mais sa réflexion avance d'un pas de budget fixe (80 unités) à CHAQUE IMAGE
// affichée (AIService::Update → 0x49B210) : selon le nombre d'images par seconde de chaque PC, elle
// finit à des instants de jeu différents (fin de tour décalée → « Turn validation mis-match »).
// On termine donc la réflexion en un seul appel. Même chose pour la file de mise à jour de la carte de l'IA (AISceneGraphService::Update
// traite 3 éléments par image) : on la vide entièrement, pour que la réflexion démarre au même moment.
#define F_AI_THINKSTEP   0x49B210u   // cdecl : étapes de réflexion tant que le budget le permet
#define AI_THINK_STATE   0x9560A8u   // 7 = réflexion terminée
#define SG_VT_UPDATE     0x825A94u   // vtable AISceneGraphService +Update(this, dt) stdcall ret 8 = 0x4B1210
#define F_SG_PROCESS     0x4B1090u   // thiscall Process(n) ret 4 (traite n éléments de la file)
#define F_GAME_PAUSED    0x4E8540u   // bool cdecl (testé avant chaque traitement)
typedef void (__cdecl *void0_t)(void);
typedef char (__cdecl *bool0_t)(void);
static void0_t o_ai_think;
static upd_t o_sg_update;

static void __cdecl h_ai_think(void) {
    int n = 0;
    do { o_ai_think(); n++; } while (*(volatile int*)AI_THINK_STATE != 7 && n < 5000);
    static int logged;
    if (logged < 30) { logged++; logf_("reflexion IA terminee en un appel (%d pas, etat %d)", n, *(volatile int*)AI_THINK_STATE); }
}

// AIService::Update (vtable 0x825D10 = 0x4B3DA0, stdcall ret 8) : quand la réflexion est terminée
// (état 7), la fin (« Thinking process has finished », lancement des actions) n'est traitée qu'à
// l'appel suivant, donc à l'image suivante. On refait l'appel dans la même image.
#define AIS_VT_UPDATE 0x825D10u
static upd_t o_ais_update;
static int __stdcall h_ais_update(uintptr_t ais, uintptr_t dt) {
    int r = o_ais_update(ais, dt);
    // début de réflexion (+0x29) → réflexion (état ≠ 7) → fin (+0x28) : tout dans la même image
    for (int i = 0; i < 3; i++) {
        int st = *(volatile int*)AI_THINK_STATE;
        if (rd8(ais + 0x29) && (st != 7 || !rd8(ais + 0x28))) r = o_ais_update(ais, dt);
        else break;
    }
    return r;
}

static int __stdcall h_sg_update(uintptr_t sg, uintptr_t dt) {
    int r = o_sg_update(sg, dt);
    int n = 0;
    while (n < 20000 && !((bool0_t)F_GAME_PAUSED)() && ((rd32(sg + 0x58) - rd32(sg + 0x54)) & ~0x1Fu)) {
        ((this1_t)F_SG_PROCESS)(sg, 0, 3);
        n++;
    }
    static int logged;
    if (n && logged < 30) { logged++; logf_("carte IA : file videe (%d lots supplementaires)", n); }
    return r;
}

// TeamControlService::AddTeamToGame(this, team, ressource, alliance)
static int __stdcall h_tc_addtogame(uintptr_t tc, uintptr_t team, uintptr_t resname, uintptr_t alliance) {
    logf_("AddTeamToGame('%s') joueur='%s' Skill=%d ressource=%s alliance=%d", sstr(rd32(team + TEAM_NAME)),
          sstr(rd32(team + TEAM_PLAYER)), (int)rd32(team + TEAM_SKILL), sstr(resname), (int)alliance);
    return o_tc_addtogame(tc, team, resname, alliance);
}

#ifndef WUM_NOLOG
// multiplexeur de log du jeu : write(this, texte, longueur, drapeau) → recopié dans le journal
static int __stdcall h_mx_write(uintptr_t mx, uintptr_t s, uintptr_t len, uintptr_t flag) {
    if (len && len < 0x10000 && rdok((void*)s, len)) log_raw((const char*)s, (int)len);
    return o_mx_write(mx, s, len, flag);
}
#endif

// ---------------------------------------------------------------- exceptions
static LONG CALLBACK veh(EXCEPTION_POINTERS* e) {
    static volatile LONG count;
    DWORD c = e->ExceptionRecord->ExceptionCode;
    if (c == 0xC0000005 || c == 0xC000001D || c == 0xC0000094 || c == 0xC00000FD || c == 0xC0000409) {
        if (InterlockedIncrement(&count) <= 20) {
            CONTEXT* x = e->ContextRecord;
            logf_("EXCEPTION %08X a EIP=%08X (adr %08X) EAX=%08X ECX=%08X EDX=%08X ESI=%08X EDI=%08X ESP=%08X",
                  (unsigned)c, (unsigned)x->Eip,
                  (unsigned)(e->ExceptionRecord->NumberParameters > 1 ? e->ExceptionRecord->ExceptionInformation[1] : 0),
                  (unsigned)x->Eax, (unsigned)x->Ecx, (unsigned)x->Edx, (unsigned)x->Esi, (unsigned)x->Edi, (unsigned)x->Esp);
            log_flush();
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------- thread de fond
static DWORD WINAPI worker(LPVOID _) {
    static const uintptr_t streams[] = {0x9626E0, 0x9626E4, 0x9626E8, 0x9626EC, 0x9626F0, 0x9626F4, 0x9626F8, 0x9626FC};
    int streams_done = 0, dbg_logged = 0;
    for (;;) {
        Sleep(250);
        // canaux de log sans sortie -> branchés sur le multiplexeur principal
        if (!streams_done && rd32(0x962718)) {
            Sleep(500);
            uintptr_t mx = rd32(0x962718);
            for (unsigned i = 0; i < sizeof streams / sizeof *streams; i++) {
                uintptr_t s = rd32(streams[i]);
                if (!s) continue;
                if (!rd32(s + 0x28) && rd32(s) == 0x8716B0 && mx && rd32(mx) == MX_VT) {
                    obj_addref(mx);
                    *(uintptr_t*)(s + 0x28) = mx;
                    logf_("canal %08X sans sortie -> branche sur %08X", (unsigned)streams[i], (unsigned)mx);
                }
            }
            streams_done = 1;
        }
        if (g_debug_trace) {
            uintptr_t cfg = rd32(DBG_CFG_PTR);
            if (cfg && rdok((void*)(cfg + 0x9A), 1)) {
                if (!(*(volatile uint8_t*)(cfg + 0x9A) & 2)) *(volatile uint8_t*)(cfg + 0x9A) |= 2;
                if (!dbg_logged) { logf_("traces debug du jeu activees"); dbg_logged = 1; }
            }
        }
        if (!netservice()) log_flush();
    }
    return 0;
}

// ---------------------------------------------------------------- installation
static void install(void) {
    static const uint8_t sig_send[] = {0x55, 0x8B, 0xEC, 0x53, 0x56};               // push ebp; mov ebp,esp; push ebx; push esi
    static const uint8_t sig_begin[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x50};        // push ebp; mov ebp,esp; sub esp,0x50
    static const uint8_t sig_refresh[] = {0x55, 0x8D, 0x6C, 0x24, 0x8C};           // push ebp; lea ebp,[esp-0x74]
    static const uint8_t sig_pop[] = {0x6A, 0xFF, 0x68, 0xA8, 0xC2, 0x7C, 0x00};     // push -1; push 0x7CC2A8
    static const uint8_t sig_jump[] = {0x0F, 0x85, 0xFF, 0x01, 0x00, 0x00};          // jne 0x4C46D3
    static const uint8_t sig_add[] = {0x8B, 0x44, 0x24, 0x04, 0x8B, 0x4C, 0x24, 0x08}; // mov eax,[esp+4]; mov ecx,[esp+8]
    static const uint8_t sig_used[] = {0x55, 0x8B, 0xEC, 0x53, 0x56};                // push ebp; mov ebp,esp; push ebx; push esi
    static const uint8_t sig_think[] = {0xD9, 0x05, 0xB0, 0x60, 0x95, 0x00};         // fld dword ptr [0x9560B0]
    int ok = 1;
    ok &= vt_patch(NS_VT + 0x18, h_ns_update, 0x70B267, (void**)&o_ns_update, "NetService::Update");
    ok &= vt_patch(NS_VT + NS_VT_MSG, h_ns_msg, 0x70C21F, (void**)&o_ns_msg, "NetService::HandleMessage");
    ok &= vt_patch(TEAMMSG_VT + 0x18, h_teammsg, 0x689CB2, (void**)&o_teammsg, "TeamMessage::Handle");
    ok &= vt_patch(TC_VT + 0x20, h_tc_addtogame, 0x73F311, (void**)&o_tc_addtogame, "TeamControlService::AddTeamToGame");
    ok &= (o_send = (thiscall1_t)make_hook(NS_SEND, h_send, sig_send, 5, "NetService::Send")) != NULL;
    ok &= (o_begingame = (thiscall1_t)make_hook(NS_BEGINGAME, h_begingame, sig_begin, 6, "NetService::ProcessBeginGameMessage")) != NULL;
    ok &= (o_refresh = (thiscall1_t)make_hook(NS_REFRESH, h_refresh, sig_refresh, 5, "NetService::RefreshLobby")) != NULL;
    {
        static const uint8_t sig_prep[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x38};          // push ebp; mov ebp,esp; sub esp,0x38
        static const uint8_t sig_popts[] = {0x6A, 0xFF, 0x68, 0x58, 0xBF, 0x7C, 0x00};   // push -1; push 0x7CBF58
        static const uint8_t sig_netjump[] = {0x0F, 0x84, 0x91, 0x00, 0x00, 0x00};       // je 0x4C093E
        if (memcmp((void*)POP_TS_NETJUMP, sig_netjump, 6) == 0) {
            DWORD old;
            VirtualProtect((void*)POP_TS_NETJUMP, 6, PAGE_EXECUTE_READWRITE, &old);
            memset((void*)POP_TS_NETJUMP, 0x90, 6);
            VirtualProtect((void*)POP_TS_NETJUMP, 6, old, &old);
            FlushInstructionCache(GetCurrentProcess(), (void*)POP_TS_NETJUMP, 6);
            o_pop_teamset = (this0_t)make_hook(F_POP_TEAMSET, h_pop_teamset, sig_popts, 7, "Remplissage fenetre d'equipe");
            o_prepare = (thiscall1_t)make_hook(F_TC_PREPARETEAM, h_prepare, sig_prep, 6, "TeamControl::PrepareTeam");
            if (!o_pop_teamset || !o_prepare) { logf_("ERREUR : fenetre d'equipe en ligne desactivee"); o_prepare = NULL; ok = 0; }
        } else { logf_("ERREUR : saut 0x4C08A7 inattendu, fenetre d'equipe en ligne desactivee"); ok = 0; }
    }
    {
        static const uint8_t sig_poplobby[] = {0x6A, 0xFF, 0x68, 0xA8, 0xC7, 0x7C, 0x00};   // push -1; push 0x7CC7A8
        o_pop_lobby = (this0_t)make_hook(F_POP_LOBBYLIST, h_pop_lobby, sig_poplobby, 7, "Remplissage liste lobby");
    }
    if (memcmp((void*)POP_SKILL_JUMP, sig_jump, 6) == 0)
        ok &= (o_pop_teams = (thiscall1_t)make_hook(F_POP_ONLINETEAM, h_pop_teams, sig_pop, 7, "PopulateOnlineTeams")) != NULL;
    else { logf_("ERREUR : filtre Skill 0x4C44CE inattendu, choix par clic desactive"); ok = 0; }
    o_add_child = (addchild_t)make_hook(F_ADD_CHILD, h_add_child, sig_add, 8, "AddChild (icone de niveau)");
    ok &= (o_tc_playerused = (thiscall1_t)make_hook(F_TC_PLAYERUSED, h_tc_playerused, sig_used, 5, "TeamControl::PlayerUsed")) != NULL;
    ok &= (o_ai_think = (void0_t)make_hook(F_AI_THINKSTEP, h_ai_think, sig_think, 6, "AI ThinkStep")) != NULL;
    ok &= vt_patch(SG_VT_UPDATE, h_sg_update, 0x4B1210, (void**)&o_sg_update, "AISceneGraphService::Update");
    ok &= vt_patch(AIS_VT_UPDATE, h_ais_update, 0x4B3DA0, (void**)&o_ais_update, "AIService::Update");
#ifndef WUM_NOLOG
    ok &= vt_patch(MX_VT + 0x10, h_mx_write, 0x642568, (void**)&o_mx_write, "LogMultiplexer::Write");
#endif
    logf_("crochets installes : %s", ok ? "tous" : "INCOMPLET (voir erreurs)");
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID _) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        GetModuleFileNameA(NULL, g_dir, MAX_PATH);
        char* sl = strrchr(g_dir, '\\'); if (sl) sl[1] = 0;
        InitializeCriticalSection(&g_cs);
#ifndef WUM_NOLOG
        char p[MAX_PATH], q[MAX_PATH];
        snprintf(p, sizeof p, "%swummod_log.txt", g_dir);
        snprintf(q, sizeof q, "%swummod_log_prec.txt", g_dir);
        DeleteFileA(q); MoveFileA(p, q);
        g_log = fopen(p, "w");
        if (g_log) setvbuf(g_log, NULL, _IOFBF, 1 << 16);
#endif
        load_ini();
        char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH);
        logf_("%s charge dans %s (base %08X)", WUM_VERSION, exe, (unsigned)(uintptr_t)GetModuleHandleA(NULL));
        logf_("gestion des IA par clic dans le lobby ; traces debug=%d", g_debug_trace);
        if ((uintptr_t)GetModuleHandleA(NULL) != 0x400000 || rd32(NS_VT + 0x18) != 0x70B267) {
            logf_("exe inattendu (pas WormsMayhem.exe build 1077) : mod inactif");
            log_flush();
            return TRUE;
        }
        AddVectoredExceptionHandler(1, veh);
        patch_crc_check();
        install();
        log_flush();
        CreateThread(NULL, 0, worker, NULL, 0, NULL);
    } else if (reason == DLL_PROCESS_DETACH) {
        logf_("fin");
        log_flush();
    }
    return TRUE;
}
