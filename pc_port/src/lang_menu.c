/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "lang_text.h"
#include "lang_pack.h"
#include "lang_ru.h"
#include "lang_jpn.h"
#include "pc_kanji.h"

#include <string.h>

#include "game.h"
#include "bodyprog/text/text_draw.h"
#include "font_region.h"
#include "main/fileinfo.h"
#include "pc_config.h"

/* Port-written menu translations (DE/FR/ES/IT). Retail PAL shipped ENGLISH
 * menus in every language (probe-verified from its OPTION.BIN/SAVELOAD.BIN)
 * — the disc has no menu strings to reuse — so this table is our own
 * translation layer, applied through the Gfx_StringDraw chokepoint: any
 * drawn string that exactly matches a key renders translated, everything
 * else (PC Options, ranking, debug) stays English. Keys are the exact
 * compiled literals, including \x07 color prefixes and \x01 kerning bytes.
 *
 * Writing rules for entries:
 * - '_' is the space stand-in, '\n' breaks lines (US menu dialect).
 * - Accents are Latin-1 bytes resolved by the PAL font (font_region.c).
 *   Uppercase accents beyond A-acute/E-acute/A-O-U-diaeresis degrade to a
 *   combining mark + '*', so translations avoid them (French caps drop
 *   accents per its own typography).
 * - A hex escape followed by a hex-digit letter must be split ("\xFC" "ber"),
 *   or C consumes the letter into the escape.
 * - NULL = keep the English text for that language. */

typedef struct {
    const char* us;
    const char* tr[5]; /* de, fr, es, it, tr-ru */
} s_MenuTranslation;

static const s_MenuTranslation s_MenuTr[] = {
    /* --- Title menu + difficulty --- */
    /* Title selection markers: '[' is byte 0x5B which the TR font repurposed
     * as Ы — retarget both brackets to the font's own mark glyph (0x5D). */
    { "[",       { NULL,    NULL,         NULL,         NULL,        "\x5D" } },
    { "]",       { NULL,    NULL,         NULL,         NULL,        "\x5D" } },
    { "LOAD",     { "LADEN",  "CHARGER",   "CARGAR",    "CARICA"    , "\x33\x41\x44\x51\x54\x33\x49\x53\x77"}},
    { "CONTINUE", { "WEITER", "CONTINUER", "CONTINUAR", "CONTINUA"  , "\x50\x51\x4F\x45\x4F\x4C\x47\x49\x53\x77"}},
    { "START",    { NULL,     "COMMENCER", "EMPEZAR",   "INIZIA"    , "\x4E\x41\x58\x41\x53\x77"}},
    { "OPTION",   { "OPTIONEN", "OPTIONS", "OPCIONES",  "OPZIONI"   , "\x4F\x50\x57\x49\x49"}},
    /* Port-added main-menu row that closes the game. Distinct key from the
     * mixed-case "Exit" below, which is the options screen's BACK button
     * (German "Zur\xFC" "ck") — these must not share a translation. */
    { "EXIT",     { "BEENDEN", "QUITTER",  "SALIR",     "ESCI"      , "\x43\x5B\x56\x4F\x45"}},
    { "EASY",     { "LEICHT", "FACILE",    "FACIL",     "FACILE"    , "\x4C\x46\x44\x4B\x4F"}},
    { "NORMAL",   { NULL,     NULL,        NULL,        "NORMALE"   , "\x4F\x42\x5B\x58\x4E\x4F"}},
    { "HARD",     { "SCHWER", "DIFFICILE", "DIFICIL",   "DIFFICILE" , "\x52\x4C\x4F\x47\x4E\x4F"}},

    /* --- Options screens --- */
    { "OPTION_\x01\x01\x01\x01\x01S",
                  { "OPTIONEN", "OPTIONS", "OPCIONES", "OPZIONI" , "\x4F\x50\x57\x49\x49"}},
    { "EXTRA_OPTION_\x01\x01\x01\x01\x01S",
                  { "EXTRA-OPTIONEN", "OPTIONS_EXTRA", "OPCIONES_EXTRA", "OPZIONI_EXTRA" , "\x45\x4F\x50\x5F\x4F\x50\x57\x49\x49"}},
    { "Exit",              { "Zur\xFC" "ck",       "Quitter",         "Salir",            "Esci"              , "\x43\x5B\x56\x4F\x45"}},
    { "Brightness_Level",  { "Helligkeit",         "Luminosit\xE9",   "Brillo",           "Luminosit\xE0"     , "\x3E\x51\x4B\x4F\x52\x53\x77"}},
    { "Controller_Config", { "Controller-Konfig.", "Config._manette", "Config._de_mando", "Config._controller" , "\x54\x50\x51\x41\x43\x4C\x46\x4E\x49\x46"}},
    { "Vibration",         { NULL,                 NULL,              "Vibraci\xF3n",     "Vibrazione"        , "\x43\x49\x42\x51\x41\x57\x49\x3E"}},
    { "Auto_Load",         { "Auto-Laden",         "Chargement_auto", "Carga_autom.",     "Caricam._auto"     , "\x41\x43\x53\x4F\x33\x41\x44\x51\x54\x33\x4B\x41"}},
    { "Language",          { "Sprache",            "Langue",          "Idioma",           "Lingua"            , "\x3E\x33\x5B\x4B"}},
    { "Sound",             { "Ton",                "Son",             "Sonido",           "Audio"             , "\x33\x43\x54\x4B"}},
    { "BGM_Volume",        { "Musiklautst\xE4rke", "Volume_musique",  "Volumen_m\xFAsica", "Volume_musica"    , "\x44\x51\x4F\x4D\x4B\x4F\x52\x53\x77\x5F\x4D\x54\x33\x5B\x4B\x49"}},
    { "SE_Volume",         { "Effektlautst.",      "Volume_effets",   "Volumen_efectos",  "Volume_effetti"    , "\x44\x51\x4F\x4D\x4B\x4F\x52\x53\x77\x5F\x3C\x55\x55\x46\x4B\x53\x4F\x43"}},
    { "Voice",             { "Stimmen",            "Voix",            "Voces",            "Voci"              , "\x44\x4F\x4C\x4F\x52"}},
    { "On",                { "Ein",                "Oui",             "S\xED",            "S\xEC"             , "\x43\x4B\x4C"}},
    { "Off",               { "Aus",                "Non",             "No",               "No"                , "\x43\x5B\x4B\x4C"}},
    { "Stereo",            { NULL,                 "St\xE9r\xE9o",    "Est\xE9reo",       NULL                , "\x52\x53\x46\x51\x46\x4F"}},
    { "Monaural",          { "Mono",               "Mono",            "Mono",             "Mono"              , "\x4D\x4F\x4E\x4F"}},

    /* --- Extra options --- */
    { "Weapon_Control", { "Waffensteuerung", "Contr\xF4le_arme", "Control_de_arma", "Controllo_arma" , "\x54\x50\x51\x41\x43\x4C\x46\x4E\x49\x46\x5F\x4F\x51\x54\x47\x49\x46\x4D"}},
    { "Blood_Color",    { "Blutfarbe",       "Couleur_sang",     "Color_de_sangre", "Colore_sangue"  , "\x57\x43\x46\x53\x5F\x4B\x51\x4F\x43\x49"}},
    { "View_Control",   { "Blicksteuerung",  "Contr\xF4le_vue",  "Control_vista",   "Controllo_vista" , "\x54\x50\x51\x41\x43\x4C\x46\x4E\x49\x46\x5F\x4B\x41\x4D\x46\x51\x4F\x4A"}},
    { "Retreat_Turn",   { "Kehrtwende",      "Demi-tour",        "Media_vuelta",    "Dietrofront"    , "\x51\x41\x33\x43\x4F\x51\x4F\x53"}},
    { "\x01W\x01a\x01l\x01k/R\x01\x01u\x01n_\x01\x01\x01\x01" "Co\x01n\x01t\x01ro\x01l",
                        { "Gehen/Rennen",    "Marche/Course",    "Andar/Correr",    "Cammina/Corri",
                          "\x56\x4F\x45\x77\x42\x41\x5F\x49\x5F\x42\x46\x44" } },
    { "Auto_Aiming",    { "Auto-Zielen",     "Vis\xE9" "e_auto", "Punter\xED" "a_auto", "Mira_autom." , "\x41\x43\x53\x4F\x50\x51\x49\x57\x46\x4C"}},
    { "View_Mode",      { "Ansichtsmodus",   "Mode_de_vue",      "Modo_de_vista",   "Modo_visuale"   , "\x51\x46\x47\x49\x4D\x5F\x4B\x41\x4D\x46\x51\x5B"}},
    { "Bullet_Adjust",  { "Munitionsbonus",  "Bonus_munitions",  "Ajuste_de_balas", "Regola_proiett." , "\x42\x4F\x4E\x54\x52\x5F\x50\x41\x53\x51\x4F\x4E\x4F\x43"}},
    { "Press",          { "Dr\xFC" "cken",   "Presser",          "Pulsar",          "Premi"          , "\x4E\x41\x47\x4D\x49"}},
    { "Switch",         { "Umschalten",      "Bascule",          "Alternar",        "Alterna"        , "\x50\x46\x51\x46\x4B\x4C\x3D\x58\x49\x53\x77"}},
    { "Normal",         { NULL,              NULL,               NULL,              "Normale"        , "\x4F\x42\x5B\x58\x4E\x5B\x4A"}},
    { "Green",          { "Gr\xFCn",         "Vert",             "Verde",           "Verde"          , "\x33\x46\x4C\x5A\x4E\x5B\x4A"}},
    { "Violet",         { "Violett",         NULL,               "Violeta",         "Viola"          , "\x55\x49\x4F\x4C\x46\x53\x4F\x43\x5B\x4A"}},
    { "Black",          { "Schwarz",         "Noir",             "Negra",           "Nero"           , "\x58\x5A\x51\x4E\x5B\x4A"}},
    { "Reverse",        { "Invertiert",      "Invers\xE9",       "Invertido",       "Invertito"      , "\x49\x4E\x43\x46\x51\x52\x49\x3E"}},
    { "Self_View",      { "Egosicht",        "Vue_subj.",        "Vista_propia",    "Soggettiva"     , "\x52\x43\x4F\x4A\x5F\x43\x49\x45"}},

    /* --- Pause --- */
    { "\x07PAUSE",  { NULL,               NULL,                 "\x07PAUSA",    "\x07PAUSA"    , "\x07\x50\x41\x54\x33\x41"}},
    { "\x07PAUSED", { "\x07PAUSIERT",     "\x07" "EN_PAUSE",    "\x07" "EN_PAUSA", "\x07IN_PAUSA" , "\x07\x50\x41\x54\x33\x41"}},

    /* --- Save dialogs (inventory save + saveload screen) --- */
    { "\x07Is_it_OK_to_save?",
        { "\x07Wirklich_speichern?", "\x07Sauvegarder_?", "\x07\xBFGuardar_partida?", "\x07Salvare?" , "\x07\x5F\x5F\x5F\x5F\x52\x69\x78\x6B\x27\x68\x63\x6D\x77\x3F"}},
    { "\x07Yes_____________No",
        { "\x07Ja____________Nein", "\x07Oui____________Non", "\x07S\xED____________No", "\x07S\xEC____________No" , "\x07\x45\x27\x5F\x5F\x5F\x5F\x5F\x5F\x5F\x5F\x5F\x5F\x5F\x5F\x5F\x4E\x48\x6D"}},
    { "\x07Yes__________No",
        { "\x07Ja_________Nein", "\x07Oui_________Non", "\x07S\xED_________No", "\x07S\xEC_________No" , "\x07\x45\x27\x5F\x5F\x5F\x5F\x5F\x5F\x5F\x5F\x5F\x5F\x4E\x48\x6D"}},
    { "NEXT_GAME_MODE",
        { "N\xC4" "CHSTER_MODUS", "MODE_SUIVANT", "SIGUIENTE_MODO", "MODO_SUCCESSIVO" , "\x52\x66\x48\x60\x6E\x79\x74\x63\x64\x5F\x6B\x48\x61\x63\x67"}},
    { "\x07Is_it_OK_to_overwrite?",
        { "\x07Wirklich_\xFC" "berschreiben?", "\x07Vraiment_\xE9" "craser_?", "\x07\xBFSobrescribir?", "\x07Sovrascrivere?" , "\x07\x5F\x5F\x5F\x5F\x50\x48\x6B\x48\x62\x27\x6A\x63\x6C\x27\x6D\x77\x3F"}},
    { "\x07Is_it_OK_to_format?",
        { "\x07Wirklich_formatieren?", "\x07Vraiment_formater_?", "\x07\xBF" "Formatear?", "\x07" "Formattare?" , "\x07\x5F\x5F\x5F\x5F\x55\x69\x6B\x67\x27\x6D\x63\x6B\x69\x5E\x27\x6D\x77\x3F"}},

    /* --- Saveload screen chrome + memory card dialogs --- */
    { "FILE", { "DATEI", "FICHIER", "ARCHIVO", NULL   , NULL}},
    { "Data", { "Daten", "Fich.",   "Datos",   "Dati" , NULL}},
    { "Save", { "Stand", "Sauv.",   "Guard",   "Salv." , NULL}},
    { "Time", { "Zeit",  "Temps",   "Hora",    "Ora"  , NULL}},
    { "You_need_1_free_block\n__to_create_a_new_file.",
        { "1_freier_Block_wird\n__ben\xF6tigt.",
          "1_bloc_libre_requis\n__pour_cr\xE9" "er_un_fichier.",
          "Se_necesita_1_bloque\n__libre_para_crear.",
          "Serve_1_blocco_libero\n__per_un_nuovo_file." , NULL}},
    { "\x07You_\x01\x01removed_\x01\x01the_\x01\x01MEMORY_\x01\x01" "CARD!",
        { "\x07Memory_Card_entfernt!", "\x07" "Carte_m\xE9moire_retir\xE9" "e_!",
          "\x07\xA1Memory_Card_retirada!", "\x07Memory_Card_rimossa!" } },
    { "\x07Now_formatting...",
        { "\x07" "Formatiere...", "\x07" "Formatage...", "\x07" "Formateando...", "\x07" "Formattazione..." , "\x07\x5F\x5F\x5F\x5F\x55\x69\x6B\x67\x27\x6D\x63\x6B\x69\x5E\x27\x68\x63\x48\x2E\x2E\x2E"}},
    { "\x07Now_saving...",
        { "\x07Speichere...", "\x07Sauvegarde...", "\x07Guardando...", "\x07Salvataggio..." , "\x07\x5F\x5F\x5F\x5F\x52\x69\x78\x6B\x27\x68\x48\x68\x63\x48\x2E\x2E\x2E"}},
    { "\x07Unable_to_create_a_new_file.",
        { "\x07" "Datei_nicht_erstellbar.", "\x07" "Cr\xE9" "ation_impossible.",
          "\x07No_se_pudo_crear.", "\x07Impossibile_creare." , "\x07\x5F\x5F\x5F\x5F\x4E\x48\x5E\x69\x62\x67\x69\x61\x68\x69\x5F\x6C\x69\x62\x60\x27\x6D\x77\x5F\x6F\x27\x64\x66\x2E"}},
    { "\x07" "Finished_saving.",
        { "\x07Gespeichert.", "\x07Sauvegarde_termin\xE9" "e.", "\x07Guardado_completado.", "\x07Salvataggio_completato." } },
    { "\x07" "Failed_to_save!",
        { "\x07Speichern_fehlgeschlagen!", "\x07" "\xC9" "chec_de_sauvegarde_!",
          "\x07\xA1" "Error_al_guardar!", "\x07Salvataggio_fallito!" } },
    { "\x07The_data_is_not_found!",
        { "\x07Keine_Daten_gefunden!", "\x07" "Donn\xE9" "es_introuvables_!",
          "\x07" "Datos_no_encontrados!", "\x07" "Dati_non_trovati!" , "\x07\x5F\x5F\x5F\x5F\x45\x27\x68\x68\x76\x48\x5F\x68\x48\x5F\x68\x27\x64\x60\x48\x68\x76\x40"}},
    { "\x07The_data_is_damaged!",
        { "\x07" "Daten_besch\xE4" "digt!", "\x07" "Donn\xE9" "es_endommag\xE9" "es_!",
          "\x07" "Datos_da\xF1" "ados!", "\x07" "Dati_danneggiati!" , "\x07\x5F\x5F\x5F\x5F\x45\x27\x68\x68\x76\x48\x5F\x6A\x69\x5E\x6B\x48\x61\x60\x48\x68\x76\x40"}},
    { "\x07" "Failed_to_load!",
        { "\x07Laden_fehlgeschlagen!", "\x07" "\xC9" "chec_du_chargement_!",
          "\x07\xA1" "Error_al_cargar!", "\x07" "Caricamento_fallito!" } },
    { "\x07" "Finished_loading.",
        { "\x07Geladen.", "\x07" "Chargement_termin\xE9.", "\x07" "Carga_completada.", "\x07" "Caricamento_completato." } },
    { "\x07Now_loading...",
        { "\x07Lade...", "\x07" "Chargement...", "\x07" "Cargando...", "\x07" "Caricamento..." , "\x07\x5F\x5F\x5F\x5F\x33\x27\x3B\x6B\x6E\x62\x65\x27\x2E\x2E\x2E"}},

    /* --- Save location names --- */
    { "Anywhere",     { "Irgendwo",      "N'importe_o\xF9", "Donde_sea",     "Ovunque"       , "\x43\x46\x33\x45\x46"}},
    { "Cafe",         { NULL,            "Caf\xE9",         "Caf\xE9",       "Caff\xE8"      , "\x4B\x41\x55\x46"}},
    { "Store",        { "Laden",         "Magasin",         "Tienda",        "Negozio"       , "\x4D\x41\x44\x41\x33\x49\x4E"}},
    { "Infirmary",    { "Krankenzimmer", "Infirmerie",      "Enfermer\xED" "a", "Infermeria" , "\x4D\x46\x45\x50\x54\x4E\x4B\x53"}},
    { "Doghouse",     { "Hundeh\xFCtte", "Niche",           "Caseta",        "Cuccia"        , "\x4B\x4F\x4E\x54\x51\x41"}},
    { "Church",       { "Kirche",        "\xC9" "glise",    "Iglesia",       "Chiesa"        , "\x57\x46\x51\x4B\x4F\x43\x77"}},
    { "Garage",       { NULL,            NULL,              "Taller",        NULL            , "\x44\x41\x51\x41\x47"}},
    { "Police",       { "Polizei",       NULL,              "Polic\xED" "a", "Polizia"       , "\x50\x4F\x4C\x49\x57\x49\x3E"}},
    { "Reception",    { "Rezeption",     "R\xE9" "ception", "Recepci\xF3n",  NULL            , "\x50\x51\x49\x5A\x4D\x4E\x41\x3E"}},
    { "Room_302",     { "Zimmer_302",    "Chambre_302",     "Cuarto_302",    "Stanza_302"    , "\x4B\x4F\x4D\x4E\x41\x53\x41\x5F\x33\x30\x32"}},
    { "Director's",   { "Direktion",     "Bureau_dir.",     "Direcci\xF3n",  "Direzione"     , "\x4B\x41\x42\x49\x4E\x46\x53\x5F\x45\x49\x51\x46\x4B\x53\x4F\x51\x41"}},
    { "Jewelry_shop", { "Juwelier",      "Bijouterie",      "Joyer\xED" "a", "Gioielleria"   , "\x3D\x43\x46\x4C\x49\x51\x4E\x5B\x4A\x5F\x4D\x41\x44\x41\x33\x49\x4E"}},
    { "Pool_hall",    { "Billardsalon",  "Billard",         "Billar",        "Sala_biliardo" , "\x42\x49\x4C\x77\x3E\x51\x45\x4E\x41\x3E"}},
    { "Antique_shop", { "Antiquit\xE4ten", "Antiquaire",    "Antig\xFC" "edades", "Antiquario" , "\x41\x4E\x53\x49\x4B\x43\x41\x51\x4E\x41\x3E\x5F\x4C\x41\x43\x4B\x41"}},
    { "Theme_park",   { "Freizeitpark",  "Parc",            "Parque",        "Luna_park"     , "\x50\x41\x51\x4B\x5F\x41\x53\x53\x51\x41\x4B\x57\x49\x4F\x4E\x4F\x43"}},
    { "Boat",         { "Boot",          "Bateau",          "Barco",         "Barca"         , "\x4C\x4F\x45\x4B\x41"}},
    { "Bridge",       { "Br\xFC" "cke",  "Pont",            "Puente",        "Ponte"         , "\x4D\x4F\x52\x53"}},
    { "Lighthouse",   { "Leuchtturm",    "Phare",           "Faro",          "Faro"          , "\x4D\x41\x3E\x4B"}},
    { "Sewer",        { "Kanal",         "\xC9" "gouts",    "Cloaca",        "Fogna"         , "\x4B\x41\x4E\x41\x4C\x49\x33\x41\x57\x49\x3E"}},
    { "Nowhere",      { "Nirgendwo",     "Nulle_part",      "Ninguna_parte", "Nessun_luogo"  , "\x4E\x49\x44\x45\x46"}},
    { "Child's_room", { "Kinderzimmer",  "Ch._enfant",      "Cuarto_ni\xF1o", "Camera_bimbo" , "\x45\x46\x53\x52\x4B\x41\x3E\x5F\x4B\x4F\x4D\x4E\x41\x53\x41"}},
    { "Next_fear",    { "N\xE4" "chste_Angst", "Peur_suivante", "Nuevo_miedo", "Nuova_paura" , "\x52\x4C\x46\x45\x54\x3D\x74\x49\x4A\x5F\x52\x53\x51\x41\x56"}},

    /* --- Inventory commands + labels --- */
    { "Use",       { "Benutzen",       "Utiliser",        "Usar",     "Usa"        , "\x43\x33\x3E\x53\x77"}},
    { "Equip",     { "Ausr\xFCsten",   "\xC9" "quiper",   "Equipar",  "Equipaggia" , "\x50\x51\x49\x4D\x46\x4E\x49\x53\x77"}},
    { "Unequip",   { "Ablegen",        "Retirer",         "Quitar",   "Rimuovi"    , "\x54\x42\x51\x41\x53\x77"}},
    { "Reload",    { "Nachladen",      "Recharger",       "Recargar", "Ricarica"   , "\x33\x41\x51\x3E\x45\x49\x53\x77"}},
    { "Detail",    { "Details",        "D\xE9" "tails",   "Detalles", "Dettagli"   , "\x49\x4E\x55\x4F"}},
    { "Look",      { "Ansehen",        "Regarder",        "Mirar",    "Guarda"     , "\x4F\x52\x4D\x4F\x53\x51\x46\x53\x77"}},
    { "Equipment", { "Ausr\xFCstung",  "\xC9" "quipement", "Equipo",  "Dotazione"  , "\x52\x4E\x41\x51\x3E\x47\x46\x4E\x49\x46"}},
    { "Option",    { "Optionen",       "Options",         "Opciones", "Opzioni"    , "\x4F\x50\x57\x49\x3E"}},
    { "Map",       { "Karte",          "Carte",           "Mapa",     "Mappa"      , "\x4B\x41\x51\x53\x41"}},
    { "Command",   { "Befehle",        "Commandes",       "Comandos", "Comandi"    , "\x4B\x4F\x4D\x41\x4E\x45\x41"}},
    { "Status",    { "Zustand",        "\xC9" "tat",      "Estado",   "Stato"      , "\x52\x53\x41\x53\x54\x52"}},
    { "Name:",     { NULL,             "Nom_:",           "Nombre:",  "Nome:"      , "\x49\x4D\x3E\x3A"}},

    /* --- PC Options (port-added screens — translated per user request; the
     * value labels shared with retail rows reuse the entries above) --- */
    { "PC_Options",        { "PC-Optionen",        "Options_PC",          "Opciones_PC",        "Opzioni_PC"     , "\x4F\x50\x57\x49\x49\x5F\x50\x43"}},
    { "Resolution",        { "Aufl\xF6sung",       "R\xE9solution",       "Resoluci\xF3n",      "Risoluzione"    , "\x51\x41\x33\x51\x46\x59\x46\x4E\x49\x46"}},
    { "Window_Mode",       { "Fenstermodus",       "Mode_fen\xEAtre",     "Modo_ventana",       "Modo_finestra"  , "\x51\x46\x47\x49\x4D\x5F\x4F\x4B\x4E\x41"}},
    { "Texture_Filter",    { "Texturfilter",       "Filtre_texture",      "Filtro_textura",     "Filtro_texture" , "\x55\x49\x4C\x77\x53\x51\x5F\x53\x46\x4B\x52\x53\x54\x51"}},
    { "Antialiasing",      { NULL,                 "Anticr\xE9nelage",    "Antialias",          "Antialias"      , "\x52\x44\x4C\x41\x47\x49\x43\x41\x4E\x49\x46"}},
    { "Post_Process",      { "Nachbearbeitung",    "Post-traitement",     "Postproceso",        "Post-processo"  , "\x50\x4F\x52\x53\x4F\x42\x51\x41\x42\x4F\x53\x4B\x41"}},
    { "Tone_Mapping",      { NULL,                 NULL,                  "Mapeo_tonal",        NULL             , "\x53\x4F\x4E\x4D\x41\x50\x50\x49\x4E\x44"}},
    /* Graphics page: its value column sits at x=196, so a label has ~132px from
     * x=64 — kept at or under "Texture_Filter", the widest English row there. */
    { "Bullet_Decals",     { "Kugell\xF6" "cher",  "Impacts_balles",      "Marcas_de_bala",     "Fori_proiett."  } },
    { "Next_Page",         { "N\xE4" "chste_Seite", "Page_suivante",      "Sig._p\xE1gina",     "Pagina_succ."   , "\x52\x4C\x46\x45\x54\x3D\x74\x41\x3E\x5F\x52\x53\x51\x41\x4E\x49\x57\x41"}},
    { "Prev_Page",         { "Vorherige_Seite",    "Page_pr\xE9" "c.",    "P\xE1gina_ant.",     "Pagina_prec."   , "\x50\x51\x46\x45\x5B\x45\x54\x74\x41\x3E\x5F\x52\x53\x51\x41\x4E\x49\x57\x41"}},
    { "Back",              { "Zur\xFC" "ck",       "Retour",              "Atr\xE1s",           "Indietro"       , "\x4E\x41\x33\x41\x45"}},
    { "FPS_Limit",         { NULL,                 "Limite_FPS",          "L\xEDmite_FPS",      "Limite_FPS"     , "\x4C\x49\x4D\x49\x53\x5F\x46\x50\x53"}},
    { "Disable_Culling",   { "Culling_aus",        "Sans_culling",        "Sin_culling",        "No_culling"     , "\x42\x46\x33\x5F\x4F\x53\x52\x46\x58\x46\x4E\x49\x3E"}},
    { "Preload_Chunks",    { "Chunks_vorladen",    "Pr\xE9" "chargement", "Precarga",           "Precarica"      , "\x50\x51\x46\x45\x33\x41\x44\x51\x54\x33\x4B\x41"}},
    { "PP_Flashlight",     { "PP-Taschenlampe",    "Lampe_PP",            "Linterna_PP",        "Torcia_PP"      , "\x55\x4F\x4E\x41\x51\x49\x4B\x5F\x50\x50"}},
    { "PP_Shadows",        { "PP-Schatten",        "Ombres_PP",           "Sombras_PP",         "Ombre_PP"       , "\x53\x46\x4E\x49\x5F\x50\x50"}},
    { "Beam_Intensity",    { "Lichtst\xE4rke",     "Intensit\xE9",        "Intensidad",         "Intensit\xE0"   , "\x3E\x51\x4B\x4F\x52\x53\x77\x5F\x4C\x54\x58\x41"}},
    { "Beam_Size",         { "Lichtkegel",         "Taille_faisceau",     "Tama\xF1o_del_haz",  "Ampiezza"       , "\x51\x41\x33\x4D\x46\x51\x5F\x4C\x54\x58\x41"}},
    { "First_Person_FOV",  { "Ego-FOV",            "FOV_1re_pers.",       "FOV_1ra_pers.",      "FOV_1a_pers."   , "\x50\x4F\x4C\x46\x5F\x33\x51\x46\x4E\x49\x3E\x5F\x4F\x53\x5F\x31\x5F\x4C\x49\x57\x41"}},
    { "Mouse_Sensitivity", { "Maus-Sensibilit\xE4t", "Sensib._souris",    "Sensib._rat\xF3n",   "Sensib._mouse"  , "\x58\x54\x43\x52\x53\x43\x49\x53\x46\x4C\x77\x4E\x4F\x52\x53\x77\x5F\x4D\x5B\x59\x49"}},
    { "Pad_Sensitivity",   { "Pad-Sensibilit\xE4t", "Sensib._manette",    "Sensib._mando",      "Sensib._pad"    , "\x58\x54\x43\x52\x53\x43\x49\x53\x46\x4C\x77\x4E\x4F\x52\x53\x77\x5F\x44\x46\x4A\x4D\x50\x41\x45\x41"}},
    { "Invert_Mouse_Y",    { "Maus_Y_invert.",     "Inverser_Y_souris",   "Invertir_Y_rat\xF3n", "Inverti_Y_mouse" , "\x49\x4E\x43\x46\x51\x52\x49\x3E\x5F\x59\x5F\x4D\x5B\x59\x49"}},
    { "Invert_Pad_Y",      { "Pad_Y_invert.",      "Inverser_Y_pad",      "Invertir_Y_mando",   "Inverti_Y_pad"  , "\x49\x4E\x43\x46\x51\x52\x49\x3E\x5F\x59\x5F\x44\x46\x4A\x4D\x50\x41\x45\x41"}},
    { "Aim_Assist",        { "Zielhilfe",          "Aide_\xE0_la_vis\xE9" "e", "Ayuda_de_apunt.", "Assist._mira"  , "\x50\x4F\x4D\x4F\x74\x77\x5F\x50\x51\x49\x5F\x50\x51\x49\x57\x46\x4C\x49\x43\x41\x4E\x49\x49"}},
    { "Crosshair",         { "Fadenkreuz",         "R\xE9ticule",         "Ret\xED" "cula",     "Mirino"         , "\x50\x51\x49\x57\x46\x4C"}},
    { "2D_Controls",       { "2D-Steuerung",       "Contr\xF4les_2D",     "Controles_2D",       "Controlli_2D"   , "\x54\x50\x51\x41\x43\x4C\x46\x4E\x49\x46\x5F\x32\x44"}},
    { "FMV_Movie_Vol",     { "FMV-Lautst\xE4rke",  "Volume_FMV",          "Volumen_FMV",        "Volume_FMV"     , "\x44\x51\x4F\x4D\x4B\x4F\x52\x53\x77\x5F\x46\x4D\x56"}},
    { "Windowed",          { "Fenster",            "Fen\xEAtre",          "Ventana",            "Finestra"       , "\x43\x5F\x4F\x4B\x4E\x46"}},
    { "Fullscreen",        { "Vollbild",           "Plein_\xE9" "cran",   "Completa",           "Intero"         , "\x50\x4F\x4C\x4E\x5B\x4A\x5F\x3C\x4B\x51\x41\x4E"}},
    { "Borderless",        { "Randlos",            "Sans_bord",           "Sin_bordes",         "Senza_bordi"    , "\x42\x46\x33\x5F\x51\x41\x4D\x4B\x49"}},
    { "Bilinear",          { NULL,                 "Bilin\xE9" "aire",    "Bilineal",           "Bilineare"      , "\x42\x49\x4C\x49\x4E\x46\x4A\x4E\x5B\x4A"}},
    { "External",          { "Extern",             "Externe",             "Externa",            "Esterna"        , "\x43\x4E\x46\x59\x4E\x49\x4A"}},
    { "In_Game",           { "Im_Spiel",           "En_jeu",              "En_juego",           "In_gioco"       , "\x43\x5F\x49\x44\x51\x46"}},
    { "Both",              { "Beide",              "Les_deux",            "Ambas",              "Entrambe"       , "\x4F\x42\x41"}},
    { "Scanlines",         { NULL,                 "Balayage",            "L\xEDneas",          "Scanline"       , "\x52\x4B\x41\x4E\x4C\x41\x4A\x4E\x5B"}},
    { "Vignette",          { NULL,                 NULL,                  "Vi\xF1" "eta",       "Vignetta"       , "\x43\x49\x4E\x77\x46\x53\x4B\x41"}},
    { "Color_Grade",       { "Farbton",            "\xC9talonnage",       "Color",              "Colore"         , "\x57\x43\x46\x53\x4F\x4B\x4F\x51\x51\x46\x4B\x57\x49\x3E"}},
    { "Film_Grain",        { "Filmkorn",           "Grain",               "Grano",              "Grana"          , "\x33\x46\x51\x4E\x4F"}},
    { "Sharpen",           { "Sch\xE4rfen",        "Nettet\xE9",          "Nitidez",            "Nitidezza"      , "\x51\x46\x33\x4B\x4F\x52\x53\x77"}},
    { "Cinematic",         { "Kino",               "Cin\xE9ma",           "Cine",               "Cinema"         , "\x4B\x49\x4E\x46\x4D\x41\x53\x4F\x44\x51\x41\x55\x49\x58\x4E\x5B\x4A"}},
    { "Filmic",            { "Filmisch",           "Filmique",            "F\xEDlmico",         "Filmico"        , "\x4B\x49\x4E\x4F"}},

    /* Rows and values added to PC Options since the block above. Label budgets
     * are the widest English row each page already fits: 15 glyphs on System,
     * 17 on Controls/Camera/HUD. Value budgets: 11 on System, 8 on HUD. */
    { "Flashlight",        { "Taschenlampe",       "Lampe",               "Linterna",           "Torcia"          } },
    { "Fog_Strength",      { "Nebelst\xE4rke",     "Brouillard",          "Intens._niebla",     "Intens._nebbia"  } },
    { "Weather_Rate",      { "Wetter-Takt",        "Fr\xE9q._m\xE9t\xE9o", "Frec._clima",       "Freq._meteo"     } },
    { "Third_Person_FOV",  { "3.-Person-FOV",      "FOV_3e_pers.",        "FOV_3ra_pers.",      "FOV_3a_pers."    } },
    { "OTS_FOV",           { "OTS-FOV",            "FOV_OTS",             "FOV_OTS",            "FOV_OTS"         } },
    { "Aim_Zoom",          { "Ziel-Zoom",          "Zoom_vis\xE9" "e",    "Zoom_apuntar",       "Zoom_mira"       } },
    { "OTS_Aim_Zoom",      { "OTS-Ziel-Zoom",      "Zoom_vis\xE9" "e_OTS", "Zoom_apunt._OTS",   "Zoom_mira_OTS"   } },
    { "OTS_Aim_In_TPS",    { "OTS-Zielen_in_TPS",  "Vis\xE9" "e_OTS_en_TPS", "Apuntar_OTS_TPS", "Mira_OTS_in_TPS" } },
    { "Camera_Collision",  { "Kamerakollision",    "Collision_cam\xE9ra", "Colisi\xF3n_c\xE1mara", "Collisione_cam." } },
    { "Minimap",           { "Minikarte",          "Mini-carte",          "Minimapa",           "Minimappa"       } },
    { "Minimap_Scale",     { "Minikarte_Gr\xF6\xDF" "e", "Taille_mini-carte", "Escala_minimapa", "Scala_minimappa" } },
    { "Minimap_Corner",    { "Minikarte_Ecke",     "Coin_mini-carte",     "Esquina_minimapa",   "Angolo_minimappa" } },
    { "Minimap_Opacity",   { "Minikarte_Deckkr.",  "Opacit\xE9_minicarte", "Opacidad_minimapa", "Opacit\xE0_minimappa" } },
    { "Minimap_Reqs_Map",  { "Nur_mit_Karte",      "Carte_requise",       "Requiere_mapa",      "Serve_la_mappa"  } },
    { "Low_HP_Glow",       { "Warnung_wenig_LP",   "Alerte_PV_bas",       "Aviso_vida_baja",    "Avviso_vita_bassa" } },
    { "Crosshair_Size",    { "Fadenkreuzgr\xF6\xDF" "e", "Taille_r\xE9ticule", "Tama\xF1o_ret\xED" "cula", "Dim._mirino" } },
    { "Text_Size",         { "Textgr\xF6\xDF" "e",  "Taille_du_texte",     "Tama\xF1o_del_texto", "Dim._testo"      } },
    { "Dither",            { NULL,                 "Tramage",             "Tramado",            "Retinatura"      } },
    { "Trilinear",         { NULL,                 "Trilin\xE9" "aire",   "Trilineal",          "Trilineare"      } },
    { "Classic",           { "Klassisch",          "Classique",           "Cl\xE1sico",         "Classico"        } },
    { "C_+_Shadows",       { "K+Schatten",         "C_+_ombres",          "C_+_sombras",        "C_+_ombre"       } },
    { "Modern",            { NULL,                 "Moderne",             "Moderno",            "Moderno"         } },
    { "M_+_Shadows",       { "M+Schatten",         "M_+_ombres",          "M_+_sombras",        "M_+_ombre"       } },
    { "Top_L",             { "Oben_L",             "Haut_G",              "Arr._Izq",           "Alto_S"          } },
    { "Top_R",             { "Oben_R",             "Haut_D",              "Arr._Der",           "Alto_D"          } },
    { "Bottom_L",          { "Unten_L",            "Bas_G",               "Ab._Izq",            "Basso_S"         } },
    { "Bottom_R",          { "Unten_R",            "Bas_D",               "Ab._Der",            "Basso_D"         } },
    { "Square",            { "Quadrat",            "Carr\xE9",            "Cuadrado",           "Quadrato"        } },
    { "Circle",            { "Kreis",              "Cercle",              "C\xEDrculo",         "Cerchio"         } },

    /* --- Inventory screen (Gfx_Inventory_ItemDescriptionDraw); the labels
     * are drawn with a 10-glyph budget. --- */
    { "Can't_use_it_here.",  { "Hier_nicht_nutzbar.", "Inutilisable_ici.", "No_se_puede_usar.", "Non_usabile_qui." } },
    { "Too_dark_to_look_at\n\t\tthe_item_here.",
        { "Zu_dunkel,_um_das\n\t\tDing_anzusehen.",
          "Trop_sombre_pour\n\t\tvoir_l'objet.",
          "Demasiado_oscuro\n\t\tpara_ver_el_objeto.",
          "Troppo_buio_per\n\t\tvedere_l'oggetto." } },
    { "Stock:",            { "Vorrat:",            "Stock_:",             "Reserva:",           "Scorta:"         } },
    { "==On==",            { "==Ein==",            "==Oui==",             "==S\xED==",          "==S\xEC=="       } },
    { "==Off==",           { "==Aus==",            "==Non==",             "==No==",             "==No=="          } },
    { "Fuel:",             { "Benzin:",            "Essence_:",           "Gasolina:",          "Benzina:"        } },

    /* --- Results screen (ranking.c Results_DisplayInfo). Labels start at
     * x=24 and their values are drawn at fixed x, so each stays inside the
     * room before its value: Total_time 13 glyphs, the distances and Items
     * 16, the shot rows (from x=72) 19, the rank 13 (its stars share the
     * line), the Defeated rows 23. --- */
    { "GAME_RESULT",        { "SPIELERGEBNIS",      "R\xC9SULTATS",        "RESULTADOS",         "RISULTATI"          } },
    { "Mode",               { "Modus",              NULL,                  "Modo",               "Modalit\xE0"        } },
    { "Saves",              { "Speicherungen",      "Sauvegardes",         "Partidas_guardadas", "Salvataggi"         } },
    { "Continues",          { "Fortsetzungen",      "Reprises",            "Continuaciones",     "Continue"           } },
    { "Total_time",         { "Gesamtzeit",         "Temps_total",         "Tiempo_total",       "Tempo_totale"       } },
    { "Walking_distance",   { "Strecke_gehend",     "Distance_march\xE9" "e", "Dist._andada",   "Distanza_a_piedi"   } },
    { "Running_distance",   { "Strecke_rennend",    "Distance_courue",     "Dist._corrida",      "Distanza_corsa"     } },
    { "Items",              { "Gegenst\xE4nde",     "Objets",              "Objetos",            "Oggetti"            } },
    { "Game_clear",         { "Durchgespielt",      "Parties_finies",      "Partidas_completadas", "Partite_completate" } },
    { "Ending",             { "Ende",               "Fin",                 "Final",              "Finale"             } },
    { "==Your_rank==",      { "==Rang==",           "==Rang==",            "==Rango==",          "==Grado=="          } },
    { "Defeated_\x01\x01" "enemy_\x01\x01" "by_\x01\x01shooting",
                            { "Gegner_erschossen",  "Ennemis_abattus_(tir)", "Abatidos_a_tiros", "Nemici_uccisi_(sparo)" } },
    { "Defeated_\x01\x01" "enemy_\x01\x01" "by_\x01\x01" "fighting",
                            { "Gegner_im_Nahkampf", "Ennemis_abattus_(m\xEAl\xE9" "e)", "Abatidos_a_golpes", "Nemici_uccisi_(lotta)" } },
    { "Shooting_style",     { "Schie\xDFstil",      "Style_de_tir",        "Estilo_de_tiro",     "Stile_di_tiro"      } },
    { "Short_range_shots",  { "Nahdistanz",         "Courte_port\xE9" "e", "A_corta_distancia",  "Corto_raggio"       } },
    { "Middle_range_shots", { "Mitteldistanz",      "Moyenne_port\xE9" "e", "A_media_distancia", "Medio_raggio"       } },
    { "Long_range_shots",   { "Ferndistanz",        "Longue_port\xE9" "e", "A_larga_distancia",  "Lungo_raggio"       } },
    { "No_aiming_shots",    { "Ohne_Zielen",        "Sans_viser",          "Sin_apuntar",        "Senza_mirare"       } },

    /* --- Paper-map prompts --- */
    { "Too_dark_to_look_at\n\t\tthe_map_here.",
        { "Zu_dunkel,_um_die\n\t\tKarte_zu_lesen.",
          "Trop_sombre_pour\n\t\tlire_la_carte.",
          "Demasiado_oscuro\n\t\tpara_ver_el_mapa.",
          "Troppo_buio_per\n\t\tleggere_la_mappa." , "\x52\x66\x63\x73\x65\x69\x67\x5F\x6D\x48\x67\x68\x69\x2C\x5F\x72\x6D\x69\x2A\x76\x0A\x09\x09\x6B\x27\x62\x3B\x66\x7A\x60\x48\x6D\x77\x5F\x72\x6D\x69\x2D\x68\x63\x2A\x6E\x60\x77\x5F\x68\x27\x5F\x65\x27\x6B\x6D\x48\x2E"}},
    { "I_don't_have_the_map\n\t\tfor_this_place.",
        { "Ich_habe_keine_Karte\n\t\tvon_diesem_Ort.",
          "Je_n'ai_pas_la_carte\n\t\tde_ce_lieu.",
          "No_tengo_el_mapa\n\t\tde_este_lugar.",
          "Non_ho_la_mappa\n\t\tdi_questo_posto." , "\x54\x5F\x67\x48\x68\x7A\x5F\x68\x48\x6D\x5F\x65\x27\x6B\x6D\x76\x0A\x09\x09\x70\x6D\x69\x3B\x69\x5F\x67\x48\x6C\x6D\x27\x2E"}},

    /* --- Language row values --- */
    { "English",
        { "Englisch", "Anglais", "Ingl\xE9s", "Inglese" } },
    { "German",
        { "Deutsch", "Allemand", "Alem\xE1n", "Tedesco" } },
    { "French",
        { "Franz\xF6sisch", "Fran\xE7" "ais", "Franc\xE9s", "Francese" } },
    { "Spanish",
        { "Spanisch", "Espagnol", "Espa\xF1ol", "Spagnolo" } },
    { "Italian",
        { "Italienisch", "Italien", "Italiano", "Italiano" } },
    { "Japanese",
        { "Japanisch", "Japonais", "Japon\xE9s", "Giapponese" } },
    { "Chinese",
        { "Chinesisch", "Chinois", "Chino", "Cinese" } },

    /* --- Inventory: whether the item can be used here (max ~10) --- */
    { "==Use_OK==",
        { "==Geht==", "==OK==", "==S\xED==", "==S\xEC==" } },
    { "==Use_OK?==",
        { "==Geht?==", "==OK?==", "==S\xED?==", "==S\xEC?==" } },
    { "==Use_NG==",
        { "==Nein==", "==Non==", "==No==", "==No==" } },

    /* The other memory-card messages were already here; this one was not. The
     * leading \x07 is the colour byte every message in that box carries. */
    { "\x07You_removed_the_MEMORY_CARD!",
        { "\x07MEMORY_CARD_entfernt!", "\x07MEMORY_CARD_retir\xE9" "e_!",
          "\x07\xA1MEMORY_CARD_retirada!", "\x07MEMORY_CARD_rimossa!" } },

    /* --- Results screen (a value follows each label) --- */
    { "Defeated_enemy_by_fighting",
        { "Im_Kampf_besiegt", "Vaincus_au_combat", "Vencidos_luchando", "Sconfitti_in_lotta" } },

    /* --- PC Options rows (max ~14..17) --- */
    { "[R]_Reset",
        { "[R]_Zur\xFC" "cksetzen", "[R]_R\xE9init.", "[R]_Reiniciar", "[R]_Ripristina" } },

    /* --- Setting values (max ~11). A technical term keeps its English. --- */
    { "VSync",                           { NULL, NULL, NULL, NULL } },
    { "PGXP",                            { NULL, NULL, NULL, NULL } },
    { "Aniso_2x",                        { NULL, NULL, NULL, NULL } },
    { "Aniso_4x",                        { NULL, NULL, NULL, NULL } },
    { "Aniso_8x",                        { NULL, NULL, NULL, NULL } },
    { "Aniso_16x",                       { NULL, NULL, NULL, NULL } },
    { "CRT",                             { NULL, NULL, NULL, NULL } },
    { "PSX_Retro",                       { NULL, NULL, NULL, NULL } },
    { "Reinhard",                        { NULL, NULL, NULL, NULL } },
    { "ACES",                            { NULL, NULL, NULL, NULL } },
    { "30_Hz",                           { NULL, NULL, NULL, NULL } },
    { "60_Hz",                           { NULL, NULL, NULL, NULL } },

    /* --- Save/load screen (raw literals, not table rows) --- */
    { "\x07________\x01New_save",
        { "\x07______Neu_speichern", "\x07_____Nouvelle_partie", "\x07_____Nueva_partida", "\x07____Nuovo_salvataggio" } },
    { "\x07____Crea\x01t\x01" "e_\x01n\x01" "e\x01w_\x01" "fi\x01le",
        { "\x07____Datei_erstellen", "\x07____Cr\xE9" "er_un_fichier", "\x07____Crear_archivo", "\x07____Crea_file" } },
    { "\x07____Fil\x01" "e_\x01\x01is_\x01\x01" "da\x01ma\x01g\x01" "ed",
        { "\x07____Datei_besch\xE4" "digt", "\x07____Fichier_endommag\xE9", "\x07____Archivo_da\xF1" "ado", "\x07____File_danneggiato" } },
    { "\x07Out_of_blocks",
        { "\x07Keine_Bl\xF6" "cke_frei", "\x07Plus_de_blocs", "\x07Sin_bloques_libres", "\x07" "Blocchi_esauriti" } },
    { "\x07No_data_file",
        { "\x07Keine_Datei", "\x07" "Aucun_fichier", "\x07Sin_archivo", "\x07Nessun_file" } },

    /* --- Save-point name --- */
    { "Bus",
        { "Bus", "Bus", "Autob\xFAs", "Autobus" } },

    /* --- Brightness screen. The trailing spaces are the layout. --- */
    { "BRIGHTNESS_",
        { "HELLIGKEIT_", "LUMINOSITE_", "BRILLO_", "LUMINOSITA_" } },
    { "CONTRAST_____",
        { "KONTRAST_____", "CONTRASTE____", "CONTRASTE____", "CONTRASTO____" } },
    { "SATURATION_",
        { "SATTIGUNG_", "SATURATION_", "SATURACION_", "SATURAZIONE_" } },

    /* --- Controller presets --- */
    { "USER",
        { "BENUTZER", "PERSO", "USUARIO", "UTENTE" } },
    { "TYPE_1",
        { "TYP_1", "TYPE_1", "TIPO_1", "TIPO_1" } },
    { "TYPE_2",
        { "TYP_2", "TYPE_2", "TIPO_2", "TIPO_2" } },
    { "TYPE_3",
        { "TYP_3", "TYPE_3", "TIPO_3", "TIPO_3" } },

    /* --- Title menu ACHIEVEMENTS row, Achievements and Controls screens. The
     * Controls action labels are right-aligned into the 129 px before their
     * colon ("Change_Camera"); wider ones pan through that space. "_Map" is that
     * screen's Map row: bare "Map" is also the PC Options start-map row. --- */
    { "ACHIEVEMENTS",    { "ERRUNGENSCHAFTEN",              "SUCCES",                           "LOGROS",                     "OBIETTIVI"               } },
    { "UNLOCKED",        { "ERREICHT",                      "OBTENU",                           "LOGRADO",                    "OTTENUTO"                } },
    { "LOCKED",          { "GESPERRT",                      "VERROUILLE",                       "BLOQUEADO",                  "BLOCCATO"                } },
    { "Point",           { "Punkt",                         NULL,                               "Punto",                      "Punto"                   } },
    { "Points",          { "Punkte",                        NULL,                               "Puntos",                     "Punti"                   } },
    { "points",          { "Punkte",                        NULL,                               "puntos",                     "punti"                   } },
    { "Gamepad_Icons",   { "Gamepad-Symbole",               "Ic\xF4nes_manette",                "Iconos_del_mando",           "Icone_controller"        } },
    { "Auto",            { NULL,                            NULL,                               "Auto.",                      NULL                      } },
    { "Function",        { "Funktion",                      "Fonction",                         "Funci\xF3n",                 "Funzione"                } },
    { "Keyboard",        { "Tastatur",                      "Clavier",                          "Teclado",                    "Tastiera"                } },
    { "Controller",      { NULL,                            "Manette",                          "Mando",                      NULL                      } },
    { "Reset",           { "Zur\xFC" "cksetzen",            "R\xE9initialiser",                 "Restablecer",                "Ripristina"              } },
    { "Reset_controls?", { "Steuerung_zur\xFC" "cksetzen?", "R\xE9initialiser_les_commandes_?", "\xBFRestablecer_controles?", "Ripristinare_i_comandi?" } },
    { "Yes",             { "Ja",                            "Oui",                              "S\xED",                      "S\xEC"                   } },
    { "No",              { "Nein",                          "Non",                              NULL,                         NULL                      } },
    { "_Map",            { "Karte",                         "Carte",                            "Mapa",                       "Mappa"                   } },
    { "Move_Up",         { "Vorw\xE4rts",                   "Avancer",                          "Avanzar",                    "Avanti"                  } },
    { "Move_Down",       { "R\xFC" "ckw\xE4rts",            "Reculer",                          "Retroceder",                 "Indietro"                } },
    { "Turn_Left",       { "Links_drehen",                  "Gauche",                           "Girar_izq.",                 "Gira_a_sin."             } },
    { "Turn_Right",      { "Rechts_drehen",                 "Droite",                           "Girar_der.",                 "Gira_a_des."             } },
    { "Action_/_Shoot",  { "Aktion/Schuss",                 "Action/Tirer",                     "Acci\xF3n/Disparo",          "Azione/Spara"            } },
    { "Run",             { "Laufen",                        "Courir",                           "Correr",                     "Corri"                   } },
    { "Sidestep_Left",   { "Schritt_links",                 "Pas_\xE0_gauche",                  "Paso_lat._izq.",             "Passo_a_sin."            } },
    { "Sidestep_Right",  { "Schritt_rechts",                "Pas_\xE0_droite",                  "Paso_lat._der.",             "Passo_a_des."            } },
    { "View",            { "Ansicht",                       "Vue",                              "Vista",                      "Visuale"                 } },
    { "Aim",             { "Zielen",                        "Viser",                            "Apuntar",                    "Mira"                    } },
    { "Pause",           { NULL,                            NULL,                               "Pausa",                      "Pausa"                   } },
    { "Inventory",       { "Inventar",                      "Inventaire",                       "Inventario",                 "Inventario"              } },
    { "Cycle_Weapons",   { "Waffe_wechseln",                "Changer_d'arme",                   "Cambiar_arma",               "Cambia_arma"             } },
    { "Quick_Heal",      { "Schnellheilung",                "Soin_rapide",                      "Cura_r\xE1pida",             "Cura_rapida"             } },
    { "Quick_Turn",      { "Schnellwende",                  "Demi-tour",                        "Giro_r\xE1pido",             "Giro_rapido"             } },
    { "Rear_Look",       { "Blick_zur\xFC" "ck",            "Regard_arri\xE8re",                "Mirar_atr\xE1s",             "Guarda_dietro"           } },
    { "Change_Camera",   { "Kamerawechsel",                 "Changer_cam.",                     "Cambio_c\xE1mara",           "Cambia_camera"           } },
    { "Swap_Shoulder",   { "Seitenwechsel",                 "Changer_c\xF4t\xE9",               "Cambio_hombro",              "Cambia_spalla"           } },
    { "Quick_Options",   { "Schnellmen\xFC",                "Menu_rapide",                      "Opc._r\xE1pidas",            "Opzioni_rapide"          } },
    { "Quick_Save",      { "Schnellspeich.",                "Sauv._rapide",                     "Guardado_r\xE1p.",           "Salv._rapido"            } },
    { "Quick_Load",      { "Schnellladen",                  "Charg._rapide",                    "Carga_r\xE1pida",            "Caric._rapido"           } },
};

int g_PcLangMenuVerbatim = 0;

const char* Pc_LangMenuText(const char* str)
{
    int i;
    int col;

    if (str == NULL)
        return str;

    /* NTSC-J serves these from the disc's own overlays — Japanese on a retail
     * disc (which is what retail draws), Chinese on the fan-translated one.
     * Not under a pack: the pack is the language then, and this branch
     * answered first, so every menu stayed English while the story was not. */
    if (g_GameRegion == Region_JPN && !Pc_LangPackActive())
    {
        const char* jp = Pc_JpnMenuText(str);
        return jp ? jp : str;
    }

    /* A Russian fan-translated disc wins over the language setting: its font
     * has no Latin letters left, so English (or German, or Polish) menus are
     * unreadable on it no matter what the config says. */
    if (Pc_RuActive())
    {
        const char* ru = Pc_RuMenuText(str);
        if (ru != NULL)
            return ru;
    }

    /* PC-side pack language (Polish): menu strings come from the pack, keyed
     * by this same US literal. A key the pack lacks keeps English. */
    if (Pc_LangPackActive())
    {
        const char* tr = Pc_LangPackMenu(str);

        if (tr != NULL)
            return tr;
        /* Not in the pack: a de/fr/es/it pack still has a column below. */
    }

    /* EUR discs always; USA only when a fan-translated disc is active (its
     * story/item text comes from the disc, these tables cover the menus the
     * patch can't reach — the port renders menus from compiled strings). And
     * any disc for a pack with a column, which is the same language. */

    /* Team-Raccoon-style fan disc (Russian): the adopted text uses the TR
     * byte encoding, so the menu translations must come from the TR column
     * (index 4), whatever the `language` config says. */
    if (g_GameRegion == Region_USA && Pc_FanTextActive() && Pc_FanTrText())
    {
        for (i = 0; i < (int)(sizeof(s_MenuTr) / sizeof(s_MenuTr[0])); i++)
        {
            if (s_MenuTr[i].us[0] == str[0] && strcmp(s_MenuTr[i].us, str) == 0)
            {
                return s_MenuTr[i].tr[4] ? s_MenuTr[i].tr[4] : str;
            }
        }
        return str;
    }

    col = Pc_LangPalColumn();
    if (col < 0 ||
        !(g_GameRegion == Region_EUR || Pc_LangPackActive() ||
          (g_GameRegion == Region_USA && Pc_FanTextActive())))
    {
        return str;
    }

    for (i = 0; i < (int)(sizeof(s_MenuTr) / sizeof(s_MenuTr[0])); i++)
    {
        if (s_MenuTr[i].us[0] == str[0] && strcmp(s_MenuTr[i].us, str) == 0)
        {
            return s_MenuTr[i].tr[col] ? s_MenuTr[i].tr[col] : str;
        }
    }

    return str;
}

/* Pixel width of a (single-line prefix of a) menu string, for the centered
 * title/difficulty entries whose US x-offsets are hardcoded constants. */
/* The glyph budget Gfx_StringDraw spends on `str`. That drawer stops on a
 * COUNT, never on the terminator, and only glyph-emitting bytes count: spaces
 * and control bytes pass for free and a Shift-JIS pair is one. Retail passes
 * fixed counts for its fixed strings; a string whose length nobody fixed, such
 * as a language pack's own menu label, has to be counted the same way or it is
 * cut (10 drew "PAL Englis" and "Russki (al") or, over-counted, read past its
 * end. Counted on the translated form, which is what the drawer walks. */
int Pc_LangMenuGlyphCount(const char* str)
{
    const unsigned char* p = (const unsigned char*)Pc_LangMenuText(str);
    int                  n = 0;

    for (; *p != 0; p++)
    {
        if (*p == '_' || *p < GLYPH_TABLE_ASCII_OFFSET)
            continue;
        if (Pc_LangSjisActive() && Pc_KanjiIsLead(*p) && p[1] != 0)
            p++;
        n++;
    }
    return n;
}

int Pc_LangMenuTextWidth(const char* str)
{
    int         width = 0;
    s_GlyphEmit emits[2];
    int         count;
    int         k;
    unsigned char c;

    for (; *str != '\0'; str++)
    {
        c = (unsigned char)*str;

        if (c == '\n')
            break;
        if (c == '_')
        {
            width += FONT_12X16_SPACE_SIZE;
            continue;
        }
        if (c == '\v')
        {
            width += 10;
            continue;
        }
        if (c == '\x01')
        {
            width -= 1;
            continue;
        }
        if (c < 0x08)
            continue; /* color codes */

        /* NTSC-J draws SJIS pairs as one 12px kanji cell (Gfx_StringDraw), so
         * measuring their bytes separately would roughly double the width. */
        if (Pc_LangSjisActive() && Pc_KanjiIsLead(c) && str[1] != '\0')
        {
            width += FONT_12X16_GLYPH_SIZE_X;
            str++;
            continue;
        }

        if (c == '!')
            c = '\\';
        else if (c == '&')
            c = '^';

        /* Same range as the drawer (see Gfx_StringDraw): the EUR atlas has cells
         * past the US strip's 'z', so measuring must not stop there either. */
        if (c >= GLYPH_TABLE_ASCII_OFFSET)
        {
            count = Font_MapChar(c, emits);
            for (k = 0; k < count; k++)
            {
                width += emits[k].advance;
            }
        }
    }

    return width;
}

/* Which column of the compiled de/fr/es/it tables this language reads.
 *
 * Those tables were written for the PAL disc, where German is language 1. The
 * PACK called "de" is the same language and wants the same menus, overlays and
 * prompts -- it carries the story and the items, and nothing else -- so it maps
 * to the same column rather than falling back to English on a US disc. Packs
 * with no column (Polish, Russian, Japanese, Chinese) carry their own and are
 * unaffected. Returns -1 when there is no column to read. */
int Pc_LangPalColumn(void)
{
    int lang = g_PcConfig.language;
    int i;

    if (lang >= 1 && lang < LANG_PACK_FIRST && lang != LANG_EN_US)
        return lang - 1;

    if (Pc_LangPackActive())
    {
        const char* code = Pc_LangIdForSlot(lang);

        for (i = 1; i < LANG_PACK_FIRST; i++)
        {
            if (i != LANG_EN_US && strcmp(code, s_LangIds[i]) == 0)
                return i - 1;
        }
    }
    return -1;
}

const char* Pc_LangMenuPal(const char* us, int lang)
{
    int i;

    /* `lang` 1..4 is the retail numbering; a pack passes -1 and the column is
     * resolved from its code instead. */
    int col = (lang >= 1 && lang <= 4) ? (lang - 1) : Pc_LangPalColumn();

    if (us == NULL || col < 0)
        return NULL;

    for (i = 0; i < (int)(sizeof(s_MenuTr) / sizeof(s_MenuTr[0])); i++)
    {
        if (s_MenuTr[i].us[0] == us[0] && strcmp(s_MenuTr[i].us, us) == 0)
            return s_MenuTr[i].tr[col];
    }
    return NULL;
}
