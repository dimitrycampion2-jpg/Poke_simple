#include <raylib.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "net.h"

/*
** Poke simple, version multijoueur.
** Menu : Campagne (solo) ou Multijoueur (heberger / rejoindre).
** Fleches : bouger.  Combat : haut/bas + ENTREE.  Echap : retour au menu.
** Le reseau (TCP) est dans net.c ; ici on envoie seulement 3 messages :
**   "start"            l'hote lance la partie
**   "pos x y dir mv"   ma position
**   "busy 0/1"         je suis en combat
**   "beaten i"         j'ai battu le dresseur i (il disparait chez l'autre)
*/

#define TILE    64
#define MAP_W   24
#define MAP_H   14
#define WIN_W   (13 * TILE)
#define WIN_H   (7 * TILE)
#define NB_TRAINERS 8

/* 1 = buisson, 0 = sol, P = depart du joueur, R = dresseur, E = sortie */
static char map[MAP_H][MAP_W + 1] = {
    "111111111111111111111111",
    "1P00000000100000000000R1",
    "100000000010011100000001",
    "10R000000000000000011101",
    "100000011100000000000001",
    "100000000000000R00000001",
    "111100000001111000000001",
    "100000000000000000R00001",
    "10000R0000000011110000R1",
    "100000000000000000000001",
    "1000111000000000000R0001",
    "100000000000R00000000001",
    "1E0000000000000000000001",
    "111111111111111111111111",
};

/* ------------------------------------------------------------ Pokemon --- */

typedef struct
{
    const char *name;
    int         dex;     /* numero : sert a trouver l'image */
    int         hp_max;
    int         atk;
} Species;

static const Species dex[] = {
    {"Pikachu",    25, 60, 14},
    {"Rattata",    19, 35,  8},
    {"Abo",        23, 40,  9},
    {"Nosferapti", 41, 38,  9},
    {"Miaouss",    52, 40, 10},
    {"Smogo",     109, 45, 10},
    {"Ectoplasma", 94, 55, 12},
};
#define NB_ENEMY_SPECIES 6   /* les 6 derniers de la liste */

typedef struct
{
    int x, y;        /* position en pixels */
    int species;     /* index dans dex[] */
    int beaten;
} Trainer;

static Trainer trainers[NB_TRAINERS];

/* ------------------------------------------------------------- etat ----- */

enum { MENU_TITLE, MENU_MULTI, MENU_HOST, MENU_JOIN, MENU_LOBBY,
       WALK, BATTLE, VICTORY };

static int   state = MENU_TITLE;
static int   quit = 0;

/* menus */
static int   menu_cur = 0;
static int   field = 0;                 /* champ actif de l'ecran "Rejoindre" */
static char  ip[40] = "127.0.0.1";
static char  port[8] = "4242";
static char  local_ip[64] = "";
static char  status[96] = "";
static int   relisten = 0;

/* partie */
static float px, py;                    /* position du joueur */
static int   dir = 0;                   /* 0 bas, 1 haut, 2 gauche, 3 droite */
static int   moving = 0;
static int   walk_tick = 0;
static int   start_x, start_y;
static int   hp;                        /* PV de Pikachu */
static int   potions = 3;
static char  toast[96];
static int   toast_time = 0;

/* multijoueur */
static int   multi = 0;                 /* 1 = partie a deux */
static int   is_host = 0;
static int   remote_on = 0;             /* l'autre joueur est la */
static int   remote_busy = 0;
static int   remote_dir = 0, remote_moving = 0, remote_tick = 0;
static Vector2 remote_pos, remote_to;
static int   sent_x, sent_y, sent_dir, sent_mv, sent_t;

/* images */
static Texture2D ground, bush, exit_tile, james, back_pikachu, front[8];
static Texture2D hero_idle[4], hero_walk[4][2];

static void say(const char *text)
{
    snprintf(toast, sizeof(toast), "%s", text);
    toast_time = 180;
}

/* ------------------------------------------------------------- carte ---- */

static int is_wall(int tx, int ty)
{
    if (tx < 0 || ty < 0 || tx >= MAP_W || ty >= MAP_H)
        return 1;
    return map[ty][tx] == '1';
}

static int blocked(float x, float y)
{
    return is_wall((int)(x + 8)  / TILE, (int)(y + 8)  / TILE)
        || is_wall((int)(x + 55) / TILE, (int)(y + 8)  / TILE)
        || is_wall((int)(x + 8)  / TILE, (int)(y + 55) / TILE)
        || is_wall((int)(x + 55) / TILE, (int)(y + 55) / TILE);
}

static int trainers_left(void)
{
    int n = 0;
    for (int i = 0; i < NB_TRAINERS; i++)
        if (!trainers[i].beaten)
            n++;
    return n;
}

/* Lit la carte : trouve le depart, les dresseurs, et les remplace par du sol */
static void load_map(void)
{
    int n = 0;
    for (int ty = 0; ty < MAP_H; ty++)
        for (int tx = 0; tx < MAP_W; tx++)
        {
            char c = map[ty][tx];
            if (c == 'P')
            {
                start_x = tx * TILE;
                start_y = ty * TILE;
            }
            if (c == 'R' && n < NB_TRAINERS)
            {
                trainers[n].x = tx * TILE;
                trainers[n].y = ty * TILE;
                /* un Pokemon different selon le dresseur (de plus en plus fort) */
                trainers[n].species = 1 + n * NB_ENEMY_SPECIES / NB_TRAINERS;
                n++;
            }
            if (c == 'P' || c == 'R')
                map[ty][tx] = '0';
        }
}

/* Nouvelle partie (solo ou a deux). L'invite demarre une case a droite. */
static void start_game(int two_players, int host)
{
    multi = two_players;
    is_host = host;
    for (int i = 0; i < NB_TRAINERS; i++)
        trainers[i].beaten = 0;
    hp = dex[0].hp_max;
    potions = 3;
    px = start_x + ((two_players && !host) ? TILE : 0);
    py = start_y;
    dir = 0;
    remote_on = two_players;
    remote_busy = 0;
    remote_pos = (Vector2){start_x + (host ? TILE : 0), start_y};
    remote_to = remote_pos;
    sent_t = 99;
    toast_time = 0;
    state = WALK;
}

/* ------------------------------------------------------------ reseau ----- */

/* Lit les messages de l'autre joueur (appele a chaque image en partie) */
static void net_pump(void)
{
    char line[NET_LINE];
    int  a, b, c, d;

    net_update();
    while (net_recv(line))
    {
        if (sscanf(line, "pos %d %d %d %d", &a, &b, &c, &d) == 4)
        {
            remote_to = (Vector2){a, b};
            remote_dir = c & 3;
            remote_moving = d;
        }
        else if (sscanf(line, "busy %d", &a) == 1)
            remote_busy = a;
        else if (sscanf(line, "beaten %d", &a) == 1 && a >= 0 && a < NB_TRAINERS
                 && !trainers[a].beaten)
        {
            trainers[a].beaten = 1;
            say("L'autre joueur a battu un dresseur !");
        }
    }
    if (remote_on && net_state() != NET_UP)
    {
        remote_on = 0;
        say("L'autre joueur s'est déconnecté");
    }
    /* l'autre joueur glisse vers sa vraie position (moins saccade) */
    if (fabsf(remote_to.x - remote_pos.x) + fabsf(remote_to.y - remote_pos.y) > 300)
        remote_pos = remote_to;
    remote_pos.x += (remote_to.x - remote_pos.x) * 0.4f;
    remote_pos.y += (remote_to.y - remote_pos.y) * 0.4f;
    remote_tick++;
}

/* Envoie ma position quand elle change (et de temps en temps) */
static void send_pos(void)
{
    if (!multi || !remote_on)
        return;
    if ((int)px != sent_x || (int)py != sent_y || dir != sent_dir
        || moving != sent_mv || ++sent_t >= 30)
    {
        net_send("pos %d %d %d %d", (int)px, (int)py, dir, moving);
        sent_x = (int)px;
        sent_y = (int)py;
        sent_dir = dir;
        sent_mv = moving;
        sent_t = 0;
    }
}

static void leave_to_title(void)
{
    net_close();
    multi = 0;
    remote_on = 0;
    state = MENU_TITLE;
    menu_cur = 0;
    status[0] = '\0';
}

/* ------------------------------------------------------------ combat ---- */

/*
** Un combat est une suite de "phases" (P_...). Chaque phase dure un certain
** nombre d'images (pt = compteur) pendant lesquelles on dessine l'animation,
** puis on passe a la phase suivante avec go().
*/
enum { P_INTRO, P_MENU, P_PATK, P_FATK, P_POTION, P_NOPOTION,
       P_FFAINT, P_PFAINT, P_WON, P_LOST };

static int   foe_i;                 /* dresseur combattu */
static int   foe_hp;
static float foe_shown, hp_shown;   /* PV affiches (descendent doucement) */
static int   phase, pt, cursor;
static int   dmg_pending;
static char  msg[96];
static int   text_t;                /* pour l'effet machine a ecrire */
static int   flash;                 /* flash blanc quand un coup touche */
static struct { char text[16]; float x, y; int t; Color c; } pop;

static void set_msg(const char *text)
{
    snprintf(msg, sizeof(msg), "%s", text);
    text_t = 0;
}

static void go(int next, const char *text)
{
    phase = next;
    pt = 0;
    if (text)
        set_msg(text);
}

static void popup(const char *text, float x, float y, Color c)
{
    snprintf(pop.text, sizeof(pop.text), "%s", text);
    pop.x = x;
    pop.y = y;
    pop.c = c;
    pop.t = 45;
}

static void start_battle(int i)
{
    foe_i = i;
    foe_hp = dex[trainers[i].species].hp_max;
    foe_shown = foe_hp;
    hp_shown = hp;
    cursor = 0;
    pop.t = 0;
    flash = 0;
    state = BATTLE;
    go(P_INTRO, TextFormat("Un dresseur te défie ! %s entre en scène !",
                           dex[trainers[i].species].name));
    if (multi)
        net_send("busy 1");
}

static void end_battle(int won)
{
    if (won)
    {
        trainers[foe_i].beaten = 1;
        if (multi)
            net_send("beaten %d", foe_i);
        say(trainers_left() ? "Gagné ! Pikachu est soigné."
                            : "Tous les dresseurs sont battus : va à la sortie !");
    }
    else
    {
        px = start_x;
        py = start_y;
        say("Pikachu est K.O. Retour au départ.");
    }
    hp = dex[0].hp_max;
    state = WALK;
    sent_t = 99;
    if (multi)
        net_send("busy 0");
}

static void chase(float *shown, int real)
{
    if (*shown > real)
        *shown = fmaxf(*shown - 0.7f, real);
    else if (*shown < real)
        *shown = fminf(*shown + 0.7f, real);
}

static void battle_update(void)
{
    const Species *e = &dex[trainers[foe_i].species];

    pt++;
    text_t++;
    chase(&foe_shown, foe_hp);
    chase(&hp_shown, hp);
    if (pop.t > 0)
    {
        pop.t--;
        pop.y -= 0.8f;
    }
    if (flash > 0)
        flash--;

    switch (phase)
    {
    case P_INTRO:
        if (pt >= 80)
            go(P_MENU, NULL);
        break;
    case P_MENU:
        if (IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_DOWN))
            cursor = !cursor;
        if (IsKeyPressed(KEY_ENTER) && cursor == 0)
        {
            dmg_pending = dex[0].atk + rand() % 7;
            go(P_PATK, "Pikachu utilise Tonnerre !");
        }
        else if (IsKeyPressed(KEY_ENTER) && potions == 0)
            go(P_NOPOTION, "Plus de potions !");
        else if (IsKeyPressed(KEY_ENTER))
        {
            potions--;
            go(P_POTION, "Tu utilises une potion !");
        }
        break;
    case P_PATK:        /* Pikachu fonce, l'éclair part, l'ennemi encaisse */
        if (pt == 26)
        {
            foe_hp -= dmg_pending;
            if (foe_hp < 0)
                foe_hp = 0;
            popup(TextFormat("-%d", dmg_pending), 640, 40, RED);
            flash = 6;
        }
        if (pt >= 80 && foe_hp <= 0)
            go(P_FFAINT, TextFormat("%s est K.O. !", e->name));
        else if (pt >= 80)
        {
            dmg_pending = e->atk - 3 + rand() % 7;
            go(P_FATK, TextFormat("%s attaque !", e->name));
        }
        break;
    case P_FATK:        /* l'ennemi fonce sur Pikachu */
        if (pt == 26)
        {
            hp -= dmg_pending;
            if (hp < 0)
                hp = 0;
            popup(TextFormat("-%d", dmg_pending), 150, 150, RED);
            flash = 6;
        }
        if (pt >= 75 && hp <= 0)
            go(P_PFAINT, "Pikachu est K.O. ...");
        else if (pt >= 75)
            go(P_MENU, NULL);
        break;
    case P_POTION:
        if (pt == 14)
        {
            hp += 25;
            if (hp > dex[0].hp_max)
                hp = dex[0].hp_max;
            popup("+25", 150, 150, GREEN);
        }
        if (pt >= 60)
        {
            dmg_pending = e->atk - 3 + rand() % 7;
            go(P_FATK, TextFormat("%s attaque !", e->name));
        }
        break;
    case P_NOPOTION:
        if (pt >= 45)
            go(P_MENU, NULL);
        break;
    case P_FFAINT:
        if (pt >= 60)
            go(P_WON, "Tu as gagné !");
        break;
    case P_PFAINT:
        if (pt >= 60)
            go(P_LOST, "Retour au départ...");
        break;
    default:            /* P_WON / P_LOST : on attend ENTREE */
        if (IsKeyPressed(KEY_ENTER))
            end_battle(phase == P_WON);
    }
}

static void draw_bar(int x, int y, int w, float value, int max)
{
    float k = value / max;
    Color c = k > 0.5f ? GREEN : k > 0.2f ? ORANGE : RED;

    DrawRectangle(x, y, w, 14, DARKGRAY);
    DrawRectangle(x, y, (int)(w * k), 14, c);
}

/* Un eclair en zigzag de a vers b (la graine change : l'eclair "crepite") */
static void draw_bolt(Vector2 a, Vector2 b, int seed)
{
    Vector2 prev = a;

    SetRandomSeed(seed);
    for (int i = 1; i <= 9; i++)
    {
        Vector2 p = {a.x + (b.x - a.x) * i / 9, a.y + (b.y - a.y) * i / 9};
        if (i < 9)
        {
            p.x += GetRandomValue(-24, 24);
            p.y += GetRandomValue(-24, 24);
        }
        DrawLineEx(prev, p, 8, (Color){255, 220, 40, 255});
        DrawLineEx(prev, p, 3, WHITE);
        prev = p;
    }
}

/* Copie les n premiers caracteres (UTF-8 : les accents comptent pour 1) */
static void prefix(char *dst, const char *src, int n)
{
    int i = 0;

    while (src[i] && n > 0)
    {
        i++;
        while ((src[i] & 0xC0) == 0x80)
            i++;
        n--;
    }
    memcpy(dst, src, i);
    dst[i] = '\0';
}

static void battle_draw(void)
{
    const Species *e = &dex[trainers[foe_i].species];
    Texture2D t = front[trainers[foe_i].species];
    Vector2 foe_off = {0, 0}, my_off = {0, 0};
    float foe_a = 1, my_a = 1;
    int foe_vis = 1, my_vis = 1;
    float s;

    /* --- decalages d'animation selon la phase --- */
    if (phase == P_INTRO)
    {
        s = 1 - fminf(pt / 45.0f, 1);
        foe_off.x = 420 * s * s;
        my_off.x = -420 * s * s;
    }
    else if (phase == P_PATK)
    {
        if (pt < 24)
            my_off = (Vector2){45 * sinf(pt / 24.0f * PI), -30 * sinf(pt / 24.0f * PI)};
        if (pt >= 26 && pt < 60)
        {
            foe_off.x = (pt % 4 < 2) ? 7 : -7;
            foe_vis = (pt / 4) % 2 == 0;
        }
    }
    else if (phase == P_FATK)
    {
        if (pt < 24)
            foe_off = (Vector2){-60 * sinf(pt / 24.0f * PI), 40 * sinf(pt / 24.0f * PI)};
        if (pt >= 26 && pt < 60)
        {
            my_off.x = (pt % 4 < 2) ? 7 : -7;
            my_vis = (pt / 4) % 2 == 0;
        }
    }
    else if (phase == P_FFAINT)
    {
        foe_off.y = pt * 3;
        foe_a = 1 - pt / 60.0f;
    }
    else if (phase == P_PFAINT)
    {
        my_off.y = pt * 3;
        my_a = 1 - pt / 60.0f;
    }

    ClearBackground((Color){248, 248, 240, 255});
    /* ombres sous les Pokemon */
    DrawEllipse(650, 215, 120, 22, (Color){205, 225, 185, 255});
    DrawEllipse(190, 318, 150, 24, (Color){205, 225, 185, 255});
    /* ennemi (en haut a droite) et Pikachu (en bas a gauche) */
    Vector2 foe_pos = {560 + foe_off.x, 30 + foe_off.y};
    Vector2 my_pos = {70 + my_off.x, 90 + my_off.y};
    if (foe_vis)
        DrawTextureEx(t, foe_pos, 0, 1.6f, Fade(WHITE, foe_a));
    if (my_vis)
        DrawTextureEx(back_pikachu, my_pos, 0, 1.2f, Fade(WHITE, my_a));
    Vector2 foe_c = {560 + t.width * 0.8f, 30 + t.height * 0.8f};
    Vector2 my_c = {70 + 115, 90 + 115};

    /* --- effets --- */
    if (phase == P_PATK && pt >= 14 && pt < 44)
    {
        draw_bolt(my_c, foe_c, pt / 3);
        draw_bolt((Vector2){my_c.x + 20, my_c.y - 20}, foe_c, pt / 3 + 50);
    }
    if (phase == P_FATK && pt >= 24 && pt < 44)   /* onde de choc sur Pikachu */
    {
        DrawCircleLines((int)my_c.x, (int)my_c.y, (pt - 24) * 5, WHITE);
        DrawCircleLines((int)my_c.x, (int)my_c.y, (pt - 24) * 3, ORANGE);
    }
    if (phase == P_POTION && pt >= 10 && pt < 55)  /* petits + verts qui montent */
        for (int i = 0; i < 6; i++)
            DrawText("+", (int)(my_c.x - 70 + i * 28),
                     (int)(my_c.y + 60 - (pt - 10) * 2 - i * 8), 30,
                     Fade(GREEN, 1 - (pt - 10) / 45.0f));
    if (flash > 0)
        DrawRectangle(0, 0, WIN_W, 320, Fade(WHITE, flash / 8.0f));
    if (pop.t > 0)
        DrawText(pop.text, (int)pop.x, (int)pop.y, 34, Fade(pop.c, pop.t / 45.0f));

    /* --- cadres d'infos --- */
    DrawRectangle(30, 25, 320, 70, BLACK);
    DrawRectangle(33, 28, 314, 64, WHITE);
    DrawText(e->name, 45, 35, 22, BLACK);
    draw_bar(45, 65, 280, foe_shown, e->hp_max);
    DrawRectangle(480, 215, 320, 80, BLACK);
    DrawRectangle(483, 218, 314, 74, WHITE);
    DrawText("Pikachu", 495, 225, 22, BLACK);
    draw_bar(495, 255, 280, hp_shown, dex[0].hp_max);
    DrawText(TextFormat("%d / %d PV", hp, dex[0].hp_max), 495, 272, 16, BLACK);

    /* --- boite de texte --- */
    DrawRectangle(0, 320, WIN_W, 128, BLACK);
    DrawRectangle(6, 326, WIN_W - 12, 116, WHITE);
    if (phase == P_MENU)
    {
        DrawText("Que doit faire Pikachu ?", 30, 340, 22, BLACK);
        DrawText(cursor == 0 ? "> Attaque" : "  Attaque", 60, 380, 24, BLACK);
        DrawText(cursor == 1 ? TextFormat("> Potion (x%d)", potions)
                             : TextFormat("  Potion (x%d)", potions), 300, 380, 24, BLACK);
    }
    else
    {
        char shown[96];
        prefix(shown, msg, text_t / 2);
        DrawText(shown, 30, 345, 22, BLACK);
        if (phase == P_WON || phase == P_LOST)
            DrawText("ENTRÉE", WIN_W - 110, 410, 18, GRAY);
    }
}

/* ----------------------------------------------------------- balade ----- */

static void walk_update(void)
{
    float nx = px;
    float ny = py;

    if (IsKeyDown(KEY_RIGHT)) { nx += 4; dir = 3; }
    if (IsKeyDown(KEY_LEFT))  { nx -= 4; dir = 2; }
    if (IsKeyDown(KEY_DOWN))  { ny += 4; dir = 0; }
    if (IsKeyDown(KEY_UP))    { ny -= 4; dir = 1; }
    moving = (nx != px || ny != py);
    if (!blocked(nx, py))
        px = nx;
    if (!blocked(px, ny))
        py = ny;
    if (moving)
        walk_tick++;
    else
        walk_tick = 0;

    /* toucher un dresseur lance le combat */
    for (int i = 0; i < NB_TRAINERS; i++)
        if (!trainers[i].beaten
            && abs((int)px - trainers[i].x) < 48 && abs((int)py - trainers[i].y) < 48)
        {
            moving = 0;
            send_pos();
            start_battle(i);
            return;
        }
    /* la sortie */
    if (map[((int)py + 32) / TILE][((int)px + 32) / TILE] == 'E')
    {
        if (trainers_left() == 0)
            state = VICTORY;
        else
        {
            say(TextFormat("Il reste %d dresseur(s) à battre !", trainers_left()));
            py -= 8;
        }
    }
    send_pos();
}

static Texture2D pick_hero(int d, int mv, int tick)
{
    return mv ? hero_walk[d][(tick / 8) % 2] : hero_idle[d];
}

static void draw_me(void)
{
    DrawTexture(pick_hero(dir, moving, walk_tick), (int)px, (int)py, WHITE);
}

static void draw_other(void)
{
    DrawTextureV(pick_hero(remote_dir, remote_moving, remote_tick),
                 remote_pos, (Color){255, 170, 140, 255});
    DrawText(is_host ? "J2" : "J1", (int)remote_pos.x + 22,
             (int)remote_pos.y - 8, 22, (Color){255, 90, 60, 255});
    if (remote_busy)
        DrawText("...", (int)remote_pos.x + 20, (int)remote_pos.y - 30, 30, WHITE);
}

static void walk_draw(void)
{
    Camera2D cam = {0};
    float cx = px + 32;
    float cy = py + 32;

    /* la camera suit le joueur sans sortir de la carte */
    if (cx < WIN_W / 2) cx = WIN_W / 2;
    if (cy < WIN_H / 2) cy = WIN_H / 2;
    if (cx > MAP_W * TILE - WIN_W / 2) cx = MAP_W * TILE - WIN_W / 2;
    if (cy > MAP_H * TILE - WIN_H / 2) cy = MAP_H * TILE - WIN_H / 2;
    cam.target = (Vector2){cx, cy};
    cam.offset = (Vector2){WIN_W / 2, WIN_H / 2};
    cam.zoom = 1;

    ClearBackground(BLACK);
    BeginMode2D(cam);
        for (int ty = 0; ty < MAP_H; ty++)
            for (int tx = 0; tx < MAP_W; tx++)
            {
                char c = map[ty][tx];
                if (c == '1')
                    DrawTexture(bush, tx * TILE, ty * TILE, WHITE);
                else if (c == 'E')
                    DrawTexture(exit_tile, tx * TILE, ty * TILE, WHITE);
                else
                    DrawTexture(ground, tx * TILE, ty * TILE, WHITE);
            }
        for (int i = 0; i < NB_TRAINERS; i++)
            if (!trainers[i].beaten)
                DrawTexture(james, trainers[i].x, trainers[i].y, WHITE);
        /* les deux joueurs : celui du dessus est dessine en premier */
        if (remote_on && remote_pos.y < py)
        {
            draw_other();
            draw_me();
        }
        else
        {
            draw_me();
            if (remote_on)
                draw_other();
        }
    EndMode2D();
    DrawRectangle(8, 8, 270, 32, (Color){0, 0, 0, 170});
    DrawText(TextFormat("Dresseurs restants : %d", trainers_left()), 16, 14, 20, WHITE);
    if (multi)
    {
        DrawRectangle(8, 44, 270, 28, (Color){0, 0, 0, 170});
        DrawText(TextFormat("%s : %s", is_host ? "J2" : "J1",
                 !remote_on ? "parti" : remote_busy ? "en combat" : "libre"),
                 16, 48, 20, WHITE);
    }
    if (toast_time > 0)
    {
        DrawRectangle(0, WIN_H - 50, WIN_W, 50, (Color){0, 0, 0, 200});
        DrawText(toast, 20, WIN_H - 38, 22, WHITE);
    }
}

/* ------------------------------------------------------------ menus ----- */

static void draw_title(const char *title, const char *sub)
{
    ClearBackground((Color){24, 40, 24, 255});
    DrawText(title, WIN_W / 2 - MeasureText(title, 48) / 2, 36, 48,
             (Color){248, 230, 120, 255});
    if (sub)
        DrawText(sub, WIN_W / 2 - MeasureText(sub, 20) / 2, 96, 20, LIGHTGRAY);
}

/* Liste verticale : off = numero d'une entree grisee (ou -1) */
static void draw_list(const char **items, int n, int y, int off)
{
    for (int i = 0; i < n; i++)
    {
        const char *s = TextFormat("%s%s", i == menu_cur ? "> " : "  ", items[i]);
        Color c = i == off ? GRAY : i == menu_cur ? WHITE : LIGHTGRAY;
        DrawText(s, WIN_W / 2 - 110, y + i * 46, 30, c);
    }
}

static void menu_move(int n)
{
    if (IsKeyPressed(KEY_DOWN) && menu_cur < n - 1)
        menu_cur++;
    if (IsKeyPressed(KEY_UP) && menu_cur > 0)
        menu_cur--;
}

/* Saisie au clavier dans un champ : chiffres et '.', retour arriere */
static void typing(char *buf, int size, int digits_only)
{
    int c = GetCharPressed();
    int len = (int)strlen(buf);

    while (c > 0)
    {
        int ok = (c >= '0' && c <= '9') || (!digits_only && (c == '.' || c == '-'
                 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')));
        if (ok && len < size - 1)
        {
            buf[len++] = (char)c;
            buf[len] = '\0';
        }
        c = GetCharPressed();
    }
    if ((IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) && len > 0)
        buf[len - 1] = '\0';
}

static void listen_now(void)
{
    int p = atoi(port);

    if (p < 1024 || p > 65535)
    {
        net_close();
        snprintf(status, sizeof(status), "Port invalide (1024-65535)");
    }
    else if (net_listen(p))
        snprintf(status, sizeof(status), "En écoute sur le port %d", p);
    else
        snprintf(status, sizeof(status), "Erreur : %s", net_error());
}

static void go_menu(int s)
{
    net_close();
    state = s;
    menu_cur = 0;
    status[0] = '\0';
}

static void menu_update(void)
{
    char before[8];
    char line[NET_LINE];
    int  up;

    if (state == MENU_TITLE)
    {
        menu_move(3);
        if (IsKeyPressed(KEY_ENTER) && menu_cur == 0)
            start_game(0, 0);
        else if (IsKeyPressed(KEY_ENTER) && menu_cur == 1)
        {
            state = MENU_MULTI;
            menu_cur = 0;
        }
        else if ((IsKeyPressed(KEY_ENTER) && menu_cur == 2) || IsKeyPressed(KEY_ESCAPE))
            quit = 1;
    }
    else if (state == MENU_MULTI)
    {
        menu_move(3);
        if (IsKeyPressed(KEY_ENTER) && menu_cur == 0)         /* heberger */
        {
            state = MENU_HOST;
            menu_cur = 0;
            net_local_ip(local_ip, sizeof(local_ip));
            listen_now();
        }
        else if (IsKeyPressed(KEY_ENTER) && menu_cur == 1)    /* rejoindre */
        {
            state = MENU_JOIN;
            field = 0;
            status[0] = '\0';
        }
        else if ((IsKeyPressed(KEY_ENTER) && menu_cur == 2) || IsKeyPressed(KEY_ESCAPE))
            go_menu(MENU_TITLE);
    }
    else if (state == MENU_HOST)
    {
        net_update();
        up = net_state() == NET_UP;
        snprintf(before, sizeof(before), "%s", port);
        if (!up)
            typing(port, 6, 1);
        else
            GetCharPressed();
        if (strcmp(before, port) != 0)          /* port modifie : on re-ecoute */
            relisten = 40;
        if (relisten > 0 && --relisten == 0)
            listen_now();
        if (net_state() == NET_FAILED && strstr(net_error(), "deconnecte"))
        {
            listen_now();
            snprintf(status, sizeof(status), "Joueur 2 déconnecté");
        }
        if (up)
            snprintf(status, sizeof(status), "Joueur 2 connecté : tu peux jouer !");
        menu_move(2);
        if (IsKeyPressed(KEY_ENTER) && menu_cur == 0 && up)
        {
            net_send("start");
            start_game(1, 1);
        }
        else if ((IsKeyPressed(KEY_ENTER) && menu_cur == 1) || IsKeyPressed(KEY_ESCAPE))
            go_menu(MENU_MULTI);
    }
    else if (state == MENU_JOIN)
    {
        net_update();
        if (net_state() == NET_UP)
        {
            state = MENU_LOBBY;
            return;
        }
        if (net_state() == NET_FAILED)
            snprintf(status, sizeof(status), "Échec : %s", net_error());
        if (net_state() == NET_CONNECTING)
        {
            if (IsKeyPressed(KEY_ESCAPE))
            {
                net_close();
                snprintf(status, sizeof(status), "Annulé");
            }
            return;
        }
        if (field == 0)
            typing(ip, sizeof(ip), 0);
        else if (field == 1)
            typing(port, sizeof(port), 1);
        if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_TAB))
            field = (field + 1) % 4;
        else if (IsKeyPressed(KEY_UP) && field > 0)
            field--;
        if (IsKeyPressed(KEY_ENTER) && field < 2)
            field++;
        else if (IsKeyPressed(KEY_ENTER) && field == 2)
        {
            if (!ip[0] || atoi(port) < 1 || atoi(port) > 65535)
                snprintf(status, sizeof(status), "IP ou port invalide");
            else if (net_connect(ip, atoi(port)))
                snprintf(status, sizeof(status), "Connexion en cours...");
            else
                snprintf(status, sizeof(status), "Erreur : %s", net_error());
        }
        if (IsKeyPressed(KEY_ESCAPE) || (IsKeyPressed(KEY_ENTER) && field == 3))
            go_menu(MENU_MULTI);
    }
    else if (state == MENU_LOBBY)       /* l'invite attend que l'hote lance */
    {
        net_update();
        while (net_recv(line))
            if (strcmp(line, "start") == 0)
            {
                start_game(1, 0);
                return;
            }
        if (net_state() != NET_UP)
        {
            go_menu(MENU_JOIN);
            snprintf(status, sizeof(status), "L'hôte a coupé la connexion");
        }
        else if (IsKeyPressed(KEY_ESCAPE))
            go_menu(MENU_JOIN);
    }
}

static void draw_field(const char *label, const char *value, int y, int active)
{
    DrawText(label, 220, y + 6, 22, LIGHTGRAY);
    DrawRectangle(330, y, 300, 36, active ? WHITE : (Color){200, 200, 190, 255});
    DrawText(TextFormat("%s%s", value, active && (GetTime() * 2 - (int)(GetTime() * 2) < 0.5) ? "_" : ""),
             338, y + 6, 24, BLACK);
}

static void draw_players(int y, int second_in)
{
    DrawText("Joueur 1 (hôte)", 220, y, 24, WHITE);
    DrawText(second_in ? "Joueur 2" : "Joueur 2  (en attente...)", 220, y + 40, 24,
             second_in ? WHITE : GRAY);
}

static void menu_draw(void)
{
    static const char *t_items[3] = {"Campagne (solo)", "Multijoueur", "Quitter"};
    static const char *m_items[3] = {"Héberger", "Rejoindre", "Retour"};
    static const char *h_items[2] = {"JOUER", "Retour"};

    if (state == MENU_TITLE)
    {
        draw_title("POKÉ SIMPLE", "Flèches + ENTRÉE");
        draw_list(t_items, 3, 170, -1);
    }
    else if (state == MENU_MULTI)
    {
        draw_title("MULTIJOUEUR", "2 joueurs, même carte, via TCP");
        draw_list(m_items, 3, 170, -1);
    }
    else if (state == MENU_HOST)
    {
        draw_title("HÉBERGER UNE PARTIE", NULL);
        DrawText(TextFormat("Ton IP : %s", local_ip), 220, 100, 24, WHITE);
        DrawText(TextFormat("Port : %s   (tape des chiffres pour le changer)", port),
                 220, 134, 20, LIGHTGRAY);
        draw_players(190, net_state() == NET_UP);
        draw_list(h_items, 2, 290, net_state() == NET_UP ? -1 : 0);
        DrawText(status, 220, 390, 20, (Color){248, 230, 120, 255});
    }
    else if (state == MENU_JOIN)
    {
        draw_title("REJOINDRE UNE PARTIE", "Tape l'IP de l'hôte et son port");
        draw_field("IP", ip, 140, field == 0);
        draw_field("Port", port, 190, field == 1);
        DrawText(field == 2 ? "> Se connecter" : "  Se connecter", 220, 260, 28,
                 field == 2 ? WHITE : LIGHTGRAY);
        DrawText(field == 3 ? "> Retour" : "  Retour", 220, 300, 28,
                 field == 3 ? WHITE : LIGHTGRAY);
        DrawText(status, 220, 370, 20, (Color){248, 230, 120, 255});
    }
    else if (state == MENU_LOBBY)
    {
        draw_title("SALON", "Connecté à l'hôte");
        draw_players(150, 1);
        DrawText("En attente que l'hôte lance la partie...", 220, 280, 22, LIGHTGRAY);
        DrawText("Échap : quitter le salon", 220, 340, 18, GRAY);
    }
}

/* ----------------------------------------------------------- musique ---- */

/*
** Deux musiques : 0 = balade (et menus), 1 = combat.
** On cherche music/monde.(ogg|mp3|wav) et music/combat.(ogg|mp3|wav).
** Tu peux mettre tes propres fichiers : ils remplacent ceux fournis.
** Sans carte son ou sans fichier, le jeu est juste muet (pas de plantage).
*/
static Music songs[2];
static int   have_song[2];
static float song_vol[2];
static int   song_now = 0;              /* la musique qu'on veut entendre */
static int   sound_ok = 0;

static void load_music(void)
{
    static const char *names[2] = {"monde", "combat"};
    static const char *ext[3] = {"ogg", "mp3", "wav"};

    InitAudioDevice();
    sound_ok = IsAudioDeviceReady();
    if (!sound_ok)
        return;
    for (int i = 0; i < 2; i++)
        for (int e = 0; e < 3 && !have_song[i]; e++)
        {
            const char *path = TextFormat("music/%s.%s", names[i], ext[e]);
            if (!FileExists(path))
                continue;
            songs[i] = LoadMusicStream(path);
            have_song[i] = IsMusicValid(songs[i]);
            if (have_song[i])
            {
                songs[i].looping = true;        /* recommence a la fin */
                SetMusicVolume(songs[i], 0);
                PlayMusicStream(songs[i]);
            }
        }
}

/* A appeler a chaque image : fondu enchaine vers la bonne musique */
static void music_update(void)
{
    if (!sound_ok)
        return;
    song_now = (state == BATTLE) ? 1 : 0;
    for (int i = 0; i < 2; i++)
    {
        if (!have_song[i])
            continue;
        float target = (i == song_now) ? 0.6f : 0.0f;
        if (song_vol[i] < target)
            song_vol[i] = fminf(song_vol[i] + 0.02f, target);
        else if (song_vol[i] > target)
            song_vol[i] = fmaxf(song_vol[i] - 0.04f, target);
        SetMusicVolume(songs[i], song_vol[i]);
        UpdateMusicStream(songs[i]);
    }
}

static void unload_music(void)
{
    for (int i = 0; i < 2; i++)
        if (have_song[i])
            UnloadMusicStream(songs[i]);
    if (sound_ok)
        CloseAudioDevice();
}

/* ------------------------------------------------------------- main ----- */

/* Se place dans le dossier du jeu (celui qui contient sprites/), meme si
** l'exe est lance depuis ailleurs (Visual Studio le lance depuis out/build/...) */
static void go_to_game_folder(void)
{
    char path[512];

    if (FileExists("sprites/tiles/ground1.png"))
        return;
    snprintf(path, sizeof(path), "%s", GetApplicationDirectory());
    for (int i = 0; i < 8; i++)
    {
        if (ChangeDirectory(path) && FileExists("sprites/tiles/ground1.png"))
            return;
        snprintf(path, sizeof(path), "%s/..", GetWorkingDirectory());
    }
}

static void load_images(void)
{
    static const char *idle[4] = {"player_front", "player_back", "player_left", "player_right"};
    static const char *walk[4][2] = {{"playerfront1", "playerfront2"},
        {"player_back1", "player_back2"}, {"player_left1", "player_left"},
        {"playerright1", "player_right"}};

    ground       = LoadTexture("sprites/tiles/ground1.png");
    bush         = LoadTexture("sprites/tiles/bush.png");
    exit_tile    = LoadTexture("sprites/tiles/exit.png");
    james        = LoadTexture("sprites/ennemy/james_front.png");
    back_pikachu = LoadTexture("sprites/battle/mon/25_back.png");
    SetTextureFilter(back_pikachu, TEXTURE_FILTER_POINT);
    for (int i = 0; i < 7; i++)
    {
        front[i] = LoadTexture(TextFormat("sprites/battle/mon/%d_front.png", dex[i].dex));
        SetTextureFilter(front[i], TEXTURE_FILTER_POINT);
    }
    for (int d = 0; d < 4; d++)
    {
        hero_idle[d] = LoadTexture(TextFormat("sprites/player/%s.png", idle[d]));
        for (int f = 0; f < 2; f++)
            hero_walk[d][f] = LoadTexture(TextFormat("sprites/player/%s.png", walk[d][f]));
    }
}

int main(void)
{
    srand(time(NULL));
    go_to_game_folder();
    InitWindow(WIN_W, WIN_H, "Poke");
    SetExitKey(KEY_NULL);               /* Echap ne ferme plus la fenetre */
    SetTargetFPS(60);
    load_images();
    load_map();
    load_music();

    while (!WindowShouldClose() && !quit)
    {
        music_update();
        if (state >= WALK && multi)
            net_pump();
        if (toast_time > 0)
            toast_time--;
        if (state < WALK)
            menu_update();
        else if (state == WALK)
        {
            walk_update();
            if (IsKeyPressed(KEY_ESCAPE))
                leave_to_title();
        }
        else if (state == BATTLE)
            battle_update();
        else if (IsKeyPressed(KEY_ESCAPE))
            leave_to_title();

        BeginDrawing();
            if (state < WALK)
                menu_draw();
            else if (state == WALK)
                walk_draw();
            else if (state == BATTLE)
                battle_draw();
            else
            {
                ClearBackground(BLACK);
                DrawText("Bravo ! Tu as battu tous les dresseurs !", 120, 190, 30, WHITE);
                DrawText("Échap : retour au menu", 300, 250, 22, GRAY);
            }
        EndDrawing();
    }
    net_close();
    unload_music();
    CloseWindow();
    return 0;
}
