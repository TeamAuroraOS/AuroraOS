#include "statusbar.h"
#include "account.h"
#include "aurora.h"
#include "anim.h"
#include "ui.h"
#include "aurora_logo.h"
#include "ff.h"
#include "font.h"
#include "keyboard.h"
#include "fwdump.h"
#include "lang.h"
#include "power.h"
#include "touch.h"
#include "user.h"

/* One row per string id (lang.h): English, Spanish, French. Accented letters
 * are \u escapes so the file stays ASCII; the pack fonts cover Latin-1. */
int g_lang = 0;

static const char *const T[STR_COUNT][LANG_COUNT] = {
    /* STR_LANGUAGE     */ {"Language", "Idioma", "Langue"},
    /* STR_NETWORK      */ {"Network", "Red", "R\u00E9seau"},
    /* STR_DETAILS      */ {"Details", "Datos", "Profil"},
    /* STR_ACCOUNT      */ {"Account", "Cuenta", "Compte"},
    /* STR_PERSONAL     */ {"Personal", "Color", "Couleur"},
    /* STR_WELCOME      */ {"Welcome", "Listo", "Pr\u00EAt"},
    /* STR_GET_STARTED  */ {"Get started", "Comenzar", "Commencer"},
    /* STR_NET_L1       */
    {"Set up a wireless network connection to use",
     "Configura una conexi\u00F3n de red inal\u00E1mbrica",
     "Configurez une connexion r\u00E9seau sans fil"},
    /* STR_NET_L2       */
    {"online features such as aShop.",
     "para usar funciones como aShop.",
     "pour les fonctions en ligne (aShop)."},
    /* STR_NET_FW       */
    {"Aurora will copy the Wi-Fi firmware first.",
     "Aurora copiar\u00E1 antes el firmware Wi-Fi.",
     "Aurora copiera d'abord le firmware Wi-Fi."},
    /* STR_SKIP         */ {"Skip", "Omitir", "Passer"},
    /* STR_NET_HINT     */
    {"A: Select   START: Next   B: Back",
     "A: Elegir   START: Siguiente   B: Atr\u00E1s",
     "A : Choisir   START : Suivant   B : Retour"},
    /* STR_USER_L1      */
    {"Enter your details so Aurora knows what to",
     "Introduce tus datos para que Aurora sepa",
     "Entrez vos infos pour qu'Aurora sache"},
    /* STR_USER_L2      */
    {"call you and when your birthday is.",
     "c\u00F3mo llamarte y tu fecha de nacimiento.",
     "comment vous appeler et votre naissance."},
    /* STR_USER_NAME    */ {"User name", "Nombre", "Nom"},
    /* STR_DAY          */ {"Day", "D\u00EDa", "Jour"},
    /* STR_MONTH        */ {"Month", "Mes", "Mois"},
    /* STR_YEAR         */ {"Year", "A\u00F1o", "Ann\u00E9e"},
    /* STR_BACK         */ {"Back", "Atr\u00E1s", "Retour"},
    /* STR_NEXT         */ {"Next", "Siguiente", "Suivant"},
    /* STR_USER_HINT    */
    {"<>: move  ^v: change  A: select", "<>: mover  ^v: cambiar  A: elegir",
     "<>: bouger ^v: changer A: choisir"},
    /* STR_KB_ENTER_NAME*/
    {"Enter your user name", "Escribe tu nombre", "Entrez votre nom"},
    /* STR_PERS_L1      */
    {"Pick an accent colour for the Aurora interface.",
     "Elige un color de acento para Aurora.",
     "Choisissez une couleur d'accent pour Aurora."},
    /* STR_PERS_HINT    */
    {"D-Pad: choose   A: Next   B: Back",
     "D-Pad: elegir  A: Sig.  B: Atr\u00E1s",
     "D-Pad: choisir A: Suiv. B: Retour"},
    /* STR_PRESS_A_START*/
    {"Press (A) to start using Aurora!",
     "\u00A1Pulsa (A) para empezar con Aurora!",
     "Appuyez sur (A) pour d\u00E9marrer Aurora !"},
    /* STR_B_BACK       */ {"B: Back", "B: Atr\u00E1s", "B: Retour"},
    /* STR_SETTINGS     */ {"Settings", "Ajustes", "R\u00E9glages"},
    /* STR_WIFI         */ {"Wi-Fi", "Wi-Fi", "Wi-Fi"},
    /* STR_ACCENT_COLOR */
    {"Accent Color", "Color de acento", "Couleur d'accent"},
    /* STR_BRIGHTNESS   */ {"Brightness", "Brillo", "Luminosit\u00E9"},
    /* STR_ABOUT        */ {"About", "Acerca de", "\u00C0 propos"},
    /* STR_OFF          */ {"Off", "Desactivado", "D\u00E9sactiv\u00E9"},
    /* STR_PICK_ACCENT  */
    {"Pick an accent colour", "Elige un color", "Choisir une couleur"},
    /* STR_A_APPLY_B_BACK*/
    {"A: Apply   B: Back", "A: Aplicar  B: Atr\u00E1s", "A: Appliquer B: Retour"},
    /* STR_POWER_OFF    */ {"Power Off", "Apagar", "\u00C9teindre"},
    /* STR_SYSTEM       */ {"System", "Sistema", "Syst\u00E8me"},
    /* STR_EMPTY_SLOT   */ {"Empty Slot", "Vac\u00EDo", "Vide"},
    /* STR_LOADING      */ {"Loading...", "Cargando...", "Chargement..."},
    /* STR_MUSIC        */ {"Music", "M\u00FAsica", "Musique"},
    /* STR_NO_TRACKS    */
    {"No .aaf files found", "No hay archivos .aaf", "Aucun fichier .aaf"},
    /* STR_PLAYING      */ {"Playing", "Reproduciendo", "Lecture"},
    /* STR_STOPPED      */ {"Stopped", "Detenido", "Arr\u00EAt\u00E9"},
    /* STR_DEBUG_CRASH  */
    {"Force Debug Crash", "Forzar Fallo", "Forcer un Crash"},
    /* STR_WIFI_TEST    */ {"Wi-Fi Test", "Prueba de Wi-Fi", "Test Wi-Fi"},
    /* STR_GPU_TEST     */ {"GPU Test", "Prueba de GPU", "Test GPU"},
    /* STR_VERSION      */ {"Version", "Versi\u00F3n", "Version"},
    /* STR_CONSOLE      */ {"Console", "Consola", "Console"},
    /* STR_BATTERY      */ {"Battery", "Bater\u00EDa", "Batterie"},
    /* STR_SD_CARD      */ {"SD card", "Tarjeta SD", "Carte SD"},
    /* STR_LICENSE      */ {"License", "Licencia", "Licence"},
    /* STR_CHARGING     */ {"charging", "cargando", "en charge"},
    /* STR_FREE         */ {"free", "libres", "libres"},
    /* STR_NO_CARD      */ {"No card", "Sin tarjeta", "Pas de carte"},
    /* STR_CORE_OLD     */
    {"An old ARM11 core is running", "N\u00FAcleo ARM11 antiguo activo",
     "Ancien coeur ARM11 actif"},
    /* STR_CORE_OLD_HINT*/
    {"Power off to load the new one", "Apaga para cargar el nuevo",
     "\u00C9teignez pour charger le nouveau"},
    /* STR_LATER        */ {"Later", "M\u00E1s tarde", "Plus tard"},
    /* STR_TOUCH_CAL    */
    {"Touch Calibration", "Calibrar pantalla", "Calibrer l'\u00E9cran"},
    /* STR_CAL_TAP      */
    {"Tap the centre of each target", "Toca el centro de cada punto",
     "Touchez le centre de chaque cible"},
    /* STR_CAL_HINT     */
    {"A stylus gives the best result.   B: Cancel",
     "Mejor con el l\u00E1piz.   B: Cancelar",
     "Utilisez le stylet.   B: Annuler"},
    /* STR_CAL_POINT    */ {"Point", "Punto", "Point"},
    /* STR_CAL_CHECK    */
    {"Draw on the screen to check", "Dibuja en la pantalla para probar",
     "Dessinez pour v\u00E9rifier"},
    /* STR_CAL_CHECK_HINT*/
    {"A: Save   X: Redo   B: Cancel", "A: Guardar  X: Repetir  B: Cancelar",
     "A: Enregistrer X: Refaire B: Annuler"},
    /* STR_CAL_RETRY    */
    {"The taps did not line up. Try again.",
     "Los toques no cuadran. Int\u00E9ntalo de nuevo.",
     "Les touches ne concordent pas. R\u00E9essayez."},
    /* STR_CAL_SAVED    */ {"Calibration saved", "Calibraci\u00F3n guardada",
                            "Calibrage enregistr\u00E9"},
    /* STR_CAL_SAVE_FAILED*/
    {"Applied, but not saved to the card", "Aplicada, pero no guardada",
     "Appliqu\u00E9, mais pas enregistr\u00E9"},
    /* STR_CUSTOM       */ {"Custom", "Propia", "Perso"},
    /* STR_DEFAULT      */ {"Default", "Est\u00E1ndar", "D\u00E9faut"},
    /* STR_MORE_INFO    */ {"More Info", "M\u00E1s info", "Plus d'infos"},
    /* STR_SOFTWARE_UPDATE*/
    {"Software Update", "Actualizaci\u00F3n", "Mise \u00E0 jour"},
    /* STR_UPDATE_NONE  */
    {"Unsupported On This Version", "No disponible", "Non disponible"},
    /* STR_EMMC_CID     */ {"eMMC CID", "eMMC CID", "eMMC CID"},
    /* STR_DEVICE       */ {"Device", "Dispositivo", "Appareil"},
    /* STR_NOT_AVAILABLE*/
    {"Not available", "No disponible", "Non disponible"},
    /* STR_CLOCK        */ {"Clock", "Reloj", "Horloge"},
    /* STR_CLOCK_HINT   */
    {"Changes Aurora's clock only, not the 3DS's",
     "Solo cambia el reloj de Aurora, no el de la 3DS",
     "Ne change que l'horloge d'Aurora, pas celle de la 3DS"},
    /* STR_CLOCK_KEYS   */
    {"Up/Down: change  A: Save  X: Reset  B: Back",
     "Arriba/Abajo: cambiar  A: Guardar  X: Borrar  B: Volver",
     "Haut/Bas: changer  A: OK  X: Effacer  B: Retour"},
    /* STR_HOUR         */ {"Hour", "Hora", "Heure"},
    /* STR_MINUTE       */ {"Minute", "Minuto", "Minute"},
    /* STR_FOLDER       */ {"Folder", "Carpeta", "Dossier"},
    /* STR_HOME         */ {"Home", "Inicio", "Accueil"},
    /* STR_ITEM         */ {"item", "elemento", "\u00E9l\u00E9ment"},
    /* STR_ITEMS        */ {"items", "elementos", "\u00E9l\u00E9ments"},
    /* STR_MOVE         */ {"Move", "Mover", "D\u00E9placer"},
    /* STR_MOVE_TO      */
    {"Move to...", "Mover a...",
     "D\u00E9placer vers..."},
    /* STR_NEW_FOLDER   */ {"New Folder", "Nueva carpeta", "Nouveau dossier"},
    /* STR_RENAME       */ {"Rename", "Renombrar", "Renommer"},
    /* STR_REMOVE_FOLDER */
    {"Remove Folder", "Quitar carpeta",
     "Retirer le dossier"},
    /* STR_CANCEL       */ {"Cancel", "Cancelar", "Annuler"},
    /* STR_POWER_ASK    */
    {"Turn off the console?", "\u00BFApagar la consola?",
     "\u00C9teindre la console ?"},
    /* STR_REMOVE_ASK   */
    {"Remove this folder?", "\u00BFQuitar esta carpeta?",
     "Retirer ce dossier ?"},
    /* STR_REMOVE_HINT  */
    {"What is in it moves out.", "Su contenido saldr\u00E1 de ella.",
     "Son contenu en sortira."},
    /* STR_FOLDER_NAME  */
    {"Folder name", "Nombre de la carpeta",
     "Nom du dossier"},
    /* STR_MOVE_KEYS    */
    {"D-pad: Move   A: Place   B: Cancel",
     "Cruceta: mover   A: colocar   B: cancelar",
     "Croix : d\u00E9placer   A : poser   B : annuler"},
    /* STR_MOVE_TOUCH   */
    {"Drop it on a folder to put it inside",
     "Su\u00E9ltalo sobre una carpeta para guardarlo",
     "D\u00E9posez-le sur un dossier pour l'y ranger"},
    /* STR_CANT_MOVE    */
    {"Can't put that there", "No se puede colocar ah\u00ED",
     "Impossible de le placer ici"},
    /* STR_ERR_FULL     */
    {"There is no room there", "No queda espacio ah\u00ED",
     "Il n'y a plus de place"},
    /* STR_ERR_DEEP     */
    {"Folders only go two deep", "Solo se permiten dos niveles de carpetas",
     "Deux niveaux de dossiers au maximum"},
    /* STR_ERR_FOLDERS  */
    {"No more folders can be made", "No se pueden crear m\u00E1s carpetas",
     "Impossible de cr\u00E9er d'autres dossiers"},
    /* STR_ERR_INSIDE   */
    {"A folder can't go inside itself",
     "Una carpeta no puede ir dentro de s\u00ED misma",
     "Un dossier ne peut pas aller en lui-m\u00EAme"},
    /* STR_NET_SETUP    */
    {"Set up Wi-Fi",
     "Configurar Wi-Fi",
     "Configurer le Wi-Fi"},
    /* STR_NET_READY    */
    {"Pick your network and connect.",
     "Elige tu red y con\u00E9ctate.",
     "Choisissez votre r\u00E9seau."},
    /* STR_NET_SAVED    */ {"Network: ", "Red: ", "R\u00E9seau : "},
    /* STR_YES          */ {"Yes", "S\u00ED", "Oui"},
    /* STR_NOT_NOW      */ {"Not now", "Ahora no", "Plus tard"},
    /* STR_OK           */ {"OK", "Aceptar", "OK"},
    /* STR_WF_SAVED_NET */
    {"Saved network: ",
     "Red guardada: ",
     "R\u00E9seau enregistr\u00E9 : "},
    /* STR_WF_WITH_PASS */
    {"with a password",
     "con contrase\u00F1a",
     "avec mot de passe"},
    /* STR_WF_OPEN      */
    {"open, no password",
     "abierta, sin contrase\u00F1a",
     "ouvert, sans mot de passe"},
    /* STR_WF_PICK_SAVED*/
    {"Pick it to connect, or search for others",
     "El\u00EDgela para conectarte o busca otras",
     "Choisissez-le, ou cherchez d'autres r\u00E9seaux"},
    /* STR_WF_NO_SAVED  */
    {"No network saved",
     "No hay ninguna red guardada",
     "Aucun r\u00E9seau enregistr\u00E9"},
    /* STR_WF_SEARCH_PICK*/
    {"Search, then pick your network",
     "Busca y elige tu red",
     "Cherchez, puis choisissez votre r\u00E9seau"},
    /* STR_WF_HINT      */
    {"A: Select   Y: Forget saved   B: Back",
     "A: Elegir   Y: Olvidar la guardada   B: Atr\u00E1s",
     "A : Choisir   Y : Oublier   B : Retour"},
    /* STR_WF_SEARCH_AGAIN*/
    {"Search again",
     "Buscar de nuevo",
     "Chercher \u00E0 nouveau"},
    /* STR_WF_SEARCH    */
    {"Search for networks",
     "Buscar redes",
     "Chercher des r\u00E9seaux"},
    /* STR_WF_SAVED_TAG */ {"Saved", "Guardada", "Enregistr\u00E9"},
    /* STR_WF_NONE_FOUND*/
    {"No networks found",
     "No se encontraron redes",
     "Aucun r\u00E9seau trouv\u00E9"},
    /* STR_WF_NONE_YET  */
    {"No networks listed yet",
     "A\u00FAn no hay redes en la lista",
     "Aucun r\u00E9seau pour l'instant"},
    /* STR_WF_P_CHIP    */
    {"Starting the Wi-Fi chip",
     "Iniciando el chip Wi-Fi",
     "D\u00E9marrage de la puce Wi-Fi"},
    /* STR_WF_P_FW      */
    {"Starting its firmware",
     "Iniciando su firmware",
     "D\u00E9marrage de son firmware"},
    /* STR_WF_P_LOOK    */
    {"Looking for networks",
     "Buscando redes",
     "Recherche des r\u00E9seaux"},
    /* STR_WF_P_JOIN    */
    {"Joining the network",
     "Conectando a la red",
     "Connexion au r\u00E9seau"},
    /* STR_WF_P_PASS    */
    {"Checking the password",
     "Comprobando la contrase\u00F1a",
     "V\u00E9rification du mot de passe"},
    /* STR_WF_P_ADDR    */
    {"Asking for an address",
     "Pidiendo una direcci\u00F3n",
     "Demande d'une adresse"},
    /* STR_WF_P_ROUTER  */
    {"Finding the router",
     "Buscando el router",
     "Recherche du routeur"},
    /* STR_WF_P_PING    */
    {"Pinging the router",
     "Haciendo ping al router",
     "Ping du routeur"},
    /* STR_WF_P_FINISH  */ {"Finishing", "Terminando", "Finalisation"},
    /* STR_WF_STILL     */
    {"Wi-Fi is still finishing",
     "El Wi-Fi a\u00FAn est\u00E1 terminando",
     "Le Wi-Fi n'a pas encore fini"},
    /* STR_WF_LEAVE     */
    {"B: Leave Wi-Fi settings",
     "B: Salir de los ajustes de Wi-Fi",
     "B : Quitter les r\u00E9glages Wi-Fi"},
    /* STR_WF_CONNECTING*/
    {"Connecting to ",
     "Conectando a ",
     "Connexion \u00E0 "},
    /* STR_WF_SEARCHING */ {"Searching", "Buscando", "Recherche"},
    /* STR_WF_WAIT      */
    {"Please wait   B twice: Stop waiting",
     "Espera   B dos veces: dejar de esperar",
     "Patientez   B deux fois : ne plus attendre"},
    /* STR_WF_R1        */
    {"network not found",
     "red no encontrada",
     "r\u00E9seau introuvable"},
    /* STR_WF_R2        */
    {"link lost",
     "conexi\u00F3n perdida",
     "liaison perdue"},
    /* STR_WF_R4        */
    {"the hotspot dropped the 3DS",
     "el punto de acceso desconect\u00F3 la 3DS",
     "le point d'acc\u00E8s a d\u00E9connect\u00E9 la 3DS"},
    /* STR_WF_R5        */
    {"authentication failed",
     "fall\u00F3 la autenticaci\u00F3n",
     "\u00E9chec de l'authentification"},
    /* STR_WF_R6        */
    {"association failed",
     "fall\u00F3 la asociaci\u00F3n",
     "\u00E9chec de l'association"},
    /* STR_WF_R7        */
    {"no resources",
     "sin recursos",
     "pas de ressources"},
    /* STR_WF_FW_STOPPED*/
    {"The Wi-Fi firmware stopped",
     "El firmware Wi-Fi se detuvo",
     "Le firmware Wi-Fi s'est arr\u00EAt\u00E9"},
    /* STR_WF_TRY       */
    {"Try again",
     "Int\u00E9ntalo de nuevo",
     "R\u00E9essayez"},
    /* STR_WF_NO_START  */
    {"The Wi-Fi chip did not start",
     "El chip Wi-Fi no arranc\u00F3",
     "La puce Wi-Fi n'a pas d\u00E9marr\u00E9"},
    /* STR_WF_TEST_HINT */
    {"Settings > Wi-Fi Test shows the chip's state",
     "Ajustes > Prueba de Wi-Fi muestra el estado del chip",
     "R\u00E9glages > Test Wi-Fi montre l'\u00E9tat de la puce"},
    /* STR_WF_SEC_WEP   */
    {"It uses WEP, which is not supported",
     "Usa WEP, que no es compatible",
     "Il utilise WEP, non pris en charge"},
    /* STR_WF_SEC_WPA1  */
    {"It uses the old WPA, not WPA2",
     "Usa el antiguo WPA, no WPA2",
     "Il utilise l'ancien WPA, pas WPA2"},
    /* STR_WF_SEC_WPA3  */
    {"It needs WPA3, which is not supported",
     "Necesita WPA3, que no es compatible",
     "Il exige WPA3, non pris en charge"},
    /* STR_WF_SEC_EAP   */
    {"It needs a user name (802.1X)",
     "Necesita un nombre de usuario (802.1X)",
     "Il exige un nom d'utilisateur (802.1X)"},
    /* STR_WF_SEC_TKIP  */
    {"It uses TKIP only, not AES",
     "Solo usa TKIP, no AES",
     "Il n'utilise que TKIP, pas AES"},
    /* STR_WF_SEC_OTHER */
    {"Its security is not supported",
     "Su seguridad no es compatible",
     "Sa s\u00E9curit\u00E9 n'est pas prise en charge"},
    /* STR_WF_ADDRESS   */ {"Address ", "Direcci\u00F3n ", "Adresse "},
    /* STR_WF_ROUTER    */ {", router ", ", router ", ", routeur "},
    /* STR_WF_PING      */ {"Ping: ", "Ping: ", "Ping : "},
    /* STR_WF_OF        */ {" of ", " de ", " sur "},
    /* STR_WF_ANSWERED  */
    {" answered, best ",
     " respondidos, mejor ",
     " r\u00E9pondus, meilleur "},
    /* STR_WF_NO_PINGS  */
    {"The router did not answer pings",
     "El router no respondi\u00F3 al ping",
     "Le routeur n'a pas r\u00E9pondu au ping"},
    /* STR_WF_CONNECTED */
    {"Connected to ",
     "Conectado a ",
     "Connect\u00E9 \u00E0 "},
    /* STR_WF_NO_ADDR   */
    {"Joined, but no address came: ",
     "Conectado, pero sin direcci\u00F3n: ",
     "Connect\u00E9, mais sans adresse : "},
    /* STR_WF_DROPPED   */
    {"The link dropped while asking",
     "La conexi\u00F3n se cort\u00F3 al pedirla",
     "La liaison a coup\u00E9 pendant la demande"},
    /* STR_WF_NO_DHCP   */
    {"The hotspot did not answer DHCP",
     "El punto de acceso no respondi\u00F3 al DHCP",
     "Le point d'acc\u00E8s n'a pas r\u00E9pondu au DHCP"},
    /* STR_WF_REASON    */ {"Reason ", "Motivo ", "Raison "},
    /* STR_WF_NOT_JOINED*/
    {"Could not join ",
     "No se pudo conectar a ",
     "Connexion impossible \u00E0 "},
    /* STR_WF_NOT_FOUND */ {"Not found: ", "No encontrada: ", "Introuvable : "},
    /* STR_WF_24GHZ     */
    {"Is it on? The 3DS only sees 2.4 GHz",
     "\u00BFEst\u00E1 encendida? La 3DS solo ve redes",
     "Est-il allum\u00E9 ? La 3DS ne voit que les"},
    /* STR_WF_CHANNELS  */
    {"networks, channels 1 to 13",
     "de 2,4 GHz, canales 1 a 13",
     "r\u00E9seaux 2,4 GHz, canaux 1 \u00E0 13"},
    /* STR_WF_BADPASS   */
    {"Wrong password? ",
     "\u00BFContrase\u00F1a incorrecta? ",
     "Mauvais mot de passe ? "},
    /* STR_WF_REFUSED   */
    {"The network did not accept it",
     "La red no la acept\u00F3",
     "Le r\u00E9seau ne l'a pas accept\u00E9"},
    /* STR_WF_PICK_CHANGE*/
    {"Pick the network again to change it",
     "Elige la red de nuevo para cambiarla",
     "Choisissez de nouveau le r\u00E9seau pour le modifier"},
    /* STR_WF_NEEDS_PASS*/
    {"Needs a password: ",
     "Necesita contrase\u00F1a: ",
     "Mot de passe requis : "},
    /* STR_WF_PICK_TYPE */
    {"Pick the network again to type it",
     "Elige la red de nuevo para escribirla",
     "Choisissez de nouveau le r\u00E9seau pour le saisir"},
    /* STR_WF_CANNOT    */
    {"Cannot join ",
     "No se puede conectar a ",
     "Connexion impossible \u00E0 "},
    /* STR_WF_JOINS     */
    {"AuroraOS joins WPA2 (and open) networks",
     "AuroraOS se conecta a redes WPA2 (y abiertas)",
     "AuroraOS rejoint les r\u00E9seaux WPA2 (et ouverts)"},
    /* STR_WF_KEYS      */
    {"The password check did not finish: ",
     "La comprobaci\u00F3n no termin\u00F3: ",
     "La v\u00E9rification n'a pas abouti : "},
    /* STR_WF_EARLY     */
    {"Joining stopped early: ",
     "La conexi\u00F3n se detuvo antes: ",
     "La connexion s'est arr\u00EAt\u00E9e t\u00F4t : "},
    /* STR_WF_SEARCHING_NETS*/
    {"Searching for networks",
     "Buscando redes",
     "Recherche des r\u00E9seaux"},
    /* STR_WF_20S       */
    {"This takes about 20 seconds",
     "Tarda unos 20 segundos",
     "Cela prend environ 20 secondes"},
    /* STR_WF_NO_FW     */
    {"No Wi-Fi firmware on the SD card",
     "No hay firmware Wi-Fi en la tarjeta SD",
     "Pas de firmware Wi-Fi sur la carte SD"},
    /* STR_WF_REOPEN    */
    {"Open Wi-Fi again to copy it from this console",
     "Vuelve a abrir Wi-Fi para copiarlo de la consola",
     "Rouvrez le Wi-Fi pour le copier depuis la console"},
    /* STR_WF_FOUND1    */
    {" network found",
     " red encontrada",
     " r\u00E9seau trouv\u00E9"},
    /* STR_WF_FOUNDN    */
    {" networks found",
     " redes encontradas",
     " r\u00E9seaux trouv\u00E9s"},
    /* STR_WF_PICK_YOURS*/
    {"Pick yours to connect",
     "Elige la tuya para conectarte",
     "Choisissez le v\u00F4tre"},
    /* STR_WF_HIDDEN    */
    {"Hidden networks are not listed",
     "Las redes ocultas no aparecen",
     "Les r\u00E9seaux masqu\u00E9s n'apparaissent pas"},
    /* STR_WF_PASS_FOR  */
    {"Password for ",
     "Contrase\u00F1a de ",
     "Mot de passe de "},
    /* STR_WF_CAPS      */
    {"L or the abc key: ABC, then symbols",
     "L o la tecla abc: ABC, luego s\u00EDmbolos",
     "L ou la touche abc : ABC, puis symboles"},
    /* STR_WF_KB_HINT   */
    {"B: Delete   START: Done   SELECT: Cancel",
     "B: Borrar   START: Listo   SELECT: Cancelar",
     "B : Effacer   START : OK   SELECT : Annuler"},
    /* STR_WF_PASSWORD  */ {"Password", "Contrase\u00F1a", "Mot de passe"},
    /* STR_WF_PASS_LEN  */
    {"A Wi-Fi password has 8 to 63 characters",
     "Una contrase\u00F1a Wi-Fi tiene de 8 a 63 caracteres",
     "Un mot de passe Wi-Fi a de 8 \u00E0 63 caract\u00E8res"},
    /* STR_WF_PASS_HEX  */
    {"(or is 64 hex digits)",
     "(o 64 d\u00EDgitos hexadecimales)",
     "(ou 64 chiffres hexad\u00E9cimaux)"},
    /* STR_WF_30S       */
    {"This takes about half a minute",
     "Tarda unos 30 segundos",
     "Cela prend environ 30 secondes"},
    /* STR_WF_NOT_SAVED */
    {"(could not save it to the SD card)",
     "(no se pudo guardar en la tarjeta SD)",
     "(impossible de l'enregistrer sur la carte SD)"},
    /* STR_WF_CAN_COPY  */
    {"Aurora can copy it from this console",
     "Aurora puede copiarlo de esta consola",
     "Aurora peut le copier depuis cette console"},
    /* STR_WF_FW_REQUIRED*/
    {"Wi-Fi firmware required",
     "Se necesita el firmware Wi-Fi",
     "Firmware Wi-Fi requis"},
    /* STR_WF_COPY_Q    */
    {"Copy it from this console?",
     "\u00BFCopiarlo de esta consola?",
     "Le copier depuis cette console ?"},
    /* STR_WF_COPIED    */
    {"Wi-Fi firmware copied",
     "Firmware Wi-Fi copiado",
     "Firmware Wi-Fi copi\u00E9"},
    /* STR_WF_FORGOT    */ {"Forgot ", "Olvidada: ", "Oubli\u00E9 : "},
    /* STR_WF_SD_FAIL   */
    {"Could not change the SD card",
     "No se pudo modificar la tarjeta SD",
     "Impossible de modifier la carte SD"},
    /* STR_FD_STEP1     */
    {"Read the system NAND",
     "Leer la NAND del sistema",
     "Lire la NAND syst\u00E8me"},
    /* STR_FD_STEP2     */
    {"Find the Wi-Fi module",
     "Buscar el m\u00F3dulo Wi-Fi",
     "Trouver le module Wi-Fi"},
    /* STR_FD_STEP3     */
    {"Decrypt the module",
     "Descifrar el m\u00F3dulo",
     "D\u00E9chiffrer le module"},
    /* STR_FD_STEP4     */
    {"Extract the firmware",
     "Extraer el firmware",
     "Extraire le firmware"},
    /* STR_FD_STEP5     */
    {"Save it to the SD card",
     "Guardarlo en la tarjeta SD",
     "L'enregistrer sur la carte SD"},
    /* STR_FD_NAND      */
    {"System NAND",
     "NAND del sistema",
     "NAND syst\u00E8me"},
    /* STR_FD_ACCESS    */
    {"This action accesses the system NAND.",
     "Esta acci\u00F3n accede a la NAND del sistema.",
     "Cette action acc\u00E8de \u00E0 la NAND syst\u00E8me."},
    /* STR_FD_PROCEED   */ {"Proceed?", "\u00BFContinuar?", "Continuer ?"},
    /* STR_FD_READ_ONLY */
    {"Aurora only reads it: nothing on the NAND changes.",
     "Aurora solo la lee: nada cambia en la NAND.",
     "Aurora la lit seulement : rien n'y est modifi\u00E9."},
    /* STR_FD_COPIED_TO */
    {"The Wi-Fi firmware is copied to SD:/Aurora/wifi.",
     "El firmware Wi-Fi se copia a SD:/Aurora/wifi.",
     "Le firmware Wi-Fi est copi\u00E9 dans SD:/Aurora/wifi."},
    /* STR_FD_ENTER     */
    {"Enter the code below, or SELECT to cancel",
     "Introduce el c\u00F3digo de abajo, o SELECT para cancelar",
     "Entrez le code ci-dessous, ou SELECT pour annuler"},
    /* STR_FD_TITLE     */
    {"Wi-Fi firmware",
     "Firmware Wi-Fi",
     "Firmware Wi-Fi"},
    /* STR_FD_COPYING   */
    {"Copying the Wi-Fi firmware",
     "Copiando el firmware Wi-Fi",
     "Copie du firmware Wi-Fi"},
    /* STR_FD_KEEP_ON   */
    {"Keep the console on",
     "No apagues la consola",
     "Laissez la console allum\u00E9e"},
    /* STR_FD_PRESS     */
    {"Press these buttons in order",
     "Pulsa estos botones en orden",
     "Appuyez sur ces boutons dans l'ordre"},
    /* STR_FD_WRONG     */
    {"Not that one: start again",
     "Ese no: empieza de nuevo",
     "Pas celui-l\u00E0 : recommencez"},
    /* STR_FD_SELECT_CANCEL*/
    {"SELECT: Cancel",
     "SELECT: Cancelar",
     "SELECT : Annuler"},
    /* STR_FD_FAILED    */
    {"Copy failed",
     "Error al copiar",
     "\u00C9chec de la copie"},
    /* STR_FD_SAVED     */
    {"Wi-Fi firmware saved",
     "Firmware Wi-Fi guardado",
     "Firmware Wi-Fi enregistr\u00E9"},
    /* STR_FD_FILES_IN  */
    {"The files are in SD:/Aurora/wifi",
     "Los archivos est\u00E1n en SD:/Aurora/wifi",
     "Les fichiers sont dans SD:/Aurora/wifi"},
    /* STR_FD_PC_WAY    */
    {"docs/wifi.md shows the way with a PC",
     "docs/wifi.md explica c\u00F3mo hacerlo con un PC",
     "docs/wifi.md explique la m\u00E9thode avec un PC"},
    /* STR_FD_E_EMMC    */
    {"The system NAND could not be read",
     "No se pudo leer la NAND del sistema",
     "Impossible de lire la NAND syst\u00E8me"},
    /* STR_FD_E_NCSD    */
    {"The system NAND has no partition table",
     "La NAND del sistema no tiene tabla de particiones",
     "La NAND syst\u00E8me n'a pas de table de partitions"},
    /* STR_FD_E_KEYS    */
    {"The system NAND did not decrypt",
     "No se pudo descifrar la NAND del sistema",
     "Impossible de d\u00E9chiffrer la NAND syst\u00E8me"},
    /* STR_FD_E_CTRNAND */
    {"CTRNAND could not be opened",
     "No se pudo abrir CTRNAND",
     "Impossible d'ouvrir CTRNAND"},
    /* STR_FD_E_NOMOD   */
    {"This console's Wi-Fi module was not found",
     "No se encontr\u00F3 el m\u00F3dulo Wi-Fi de la consola",
     "Module Wi-Fi de la console introuvable"},
    /* STR_FD_E_READ    */
    {"The Wi-Fi module could not be read",
     "No se pudo leer el m\u00F3dulo Wi-Fi",
     "Impossible de lire le module Wi-Fi"},
    /* STR_FD_E_CRYPT   */
    {"The module uses encryption Aurora cannot undo",
     "El m\u00F3dulo usa un cifrado que Aurora no puede deshacer",
     "Le module utilise un chiffrement qu'Aurora ne sait pas d\u00E9faire"},
    /* STR_FD_E_DECRYPT */
    {"The module did not decrypt",
     "No se pudo descifrar el m\u00F3dulo",
     "Impossible de d\u00E9chiffrer le module"},
    /* STR_FD_E_NOCODE  */
    {"The module has no code",
     "El m\u00F3dulo no tiene c\u00F3digo",
     "Le module n'a pas de code"},
    /* STR_FD_E_UNPACK  */
    {"The module's code did not unpack",
     "No se pudo descomprimir el c\u00F3digo del m\u00F3dulo",
     "Impossible de d\u00E9compresser le code du module"},
    /* STR_FD_E_NOFW    */
    {"No Wi-Fi firmware was found in the module",
     "No se encontr\u00F3 firmware Wi-Fi en el m\u00F3dulo",
     "Aucun firmware Wi-Fi trouv\u00E9 dans le module"},
    /* STR_FD_E_WRITE   */
    {"Could not write to the SD card",
     "No se pudo escribir en la tarjeta SD",
     "Impossible d'\u00E9crire sur la carte SD"},
    /* STR_ST_UPDATE1   */ {" new update)", " actualizaci\u00F3n)",
                            " mise \u00E0 jour)"},
    /* STR_ST_UPDATES   */ {" new updates)", " actualizaciones)",
                            " mises \u00E0 jour)"},
    /* STR_ST_DL_BAR    */
    {"aShop (Downloading software...)",
     "aShop (Descargando software...)",
     "aShop (T\u00E9l\u00E9chargement...)"},
    /* STR_ST_SEARCH    */ {"Search", "Buscar", "Chercher"},
    /* STR_ST_GO        */ {"Go!", "Entrar", "Ouvrir"},
    /* STR_ST_OPTIONS   */ {"Options", "Opciones", "Options"},
    /* STR_ST_DOWNLOAD  */ {"Download", "Descargar", "T\u00E9l\u00E9charger"},
    /* STR_ST_UPDATE    */ {"Update", "Actualizar", "Mettre \u00E0 jour"},
    /* STR_ST_AGAIN     */
    {"Download again",
     "Descargar de nuevo",
     "Ret\u00E9l\u00E9charger"},
    /* STR_ST_WELCOME   */
    {"Welcome to aShop!",
     "\u00A1Te damos la bienvenida a aShop!",
     "Bienvenue sur aShop !"},
    /* STR_ST_SEARCH_TITLE*/
    {"Search aShop",
     "Buscar en aShop",
     "Chercher dans aShop"},
    /* STR_ST_KB_HINT   */
    {"B: Delete   L: Caps   START: Search   SELECT: Cancel",
     "B: Borrar   L: May\u00FAs.   START: Buscar   SELECT: Cancelar",
     "B : Effacer   L : Maj   START : Chercher   SELECT : Annuler"},
    /* STR_ST_APP_NAME  */
    {"Name of an app",
     "Nombre de una app",
     "Nom d'une app"},
    /* STR_ST_EMPTY     */
    {"Nothing in the catalogue",
     "El cat\u00E1logo est\u00E1 vac\u00EDo",
     "Le catalogue est vide"},
    /* STR_ST_INSTALLED */ {"Installed", "Instalada", "Install\u00E9e"},
    /* STR_ST_UPDATE_TAG*/ {"Update", "Actualizar", "Mise \u00E0 jour"},
    /* STR_ST_VERSION   */ {"Version", "Versi\u00F3n", "Version"},
    /* STR_ST_SIZE      */ {"Size", "Tama\u00F1o", "Taille"},
    /* STR_ST_STATUS    */ {"Status", "Estado", "\u00C9tat"},
    /* STR_ST_UPD_AVAIL */
    {"Update available",
     "Actualizaci\u00F3n disponible",
     "Mise \u00E0 jour disponible"},
    /* STR_ST_NOT_INST  */
    {"Not installed",
     "No instalada",
     "Non install\u00E9e"},
    /* STR_ST_SCROLL    */
    {"Up and Down scroll the page",
     "Arriba y Abajo desplazan la p\u00E1gina",
     "Haut et Bas font d\u00E9filer la page"},
    /* STR_ST_DOWNLOADING*/
    {"Downloading",
     "Descargando",
     "T\u00E9l\u00E9chargement"},
    /* STR_ST_DL_PREFIX */
    {"Downloading... (",
     "Descargando... (",
     "T\u00E9l\u00E9chargement... ("},
    /* STR_ST_DL_FAILED */
    {"Download failed",
     "Error en la descarga",
     "\u00C9chec du t\u00E9l\u00E9chargement"},
    /* STR_ST_ON_HOME   */
    {"is on the Home Menu",
     "ya est\u00E1 en el men\u00FA de inicio",
     "est dans le menu d'accueil"},
    /* STR_ST_E_NOSD    */
    {"There is no SD card",
     "No hay tarjeta SD",
     "Pas de carte SD"},
    /* STR_ST_E_INSTALL */
    {"Could not install the app",
     "No se pudo instalar la app",
     "Impossible d'installer l'app"},
    /* STR_ST_E_FULL    */
    {"The SD card is full",
     "La tarjeta SD est\u00E1 llena",
     "La carte SD est pleine"},
    /* STR_ST_NO_UPDATES*/
    {"No updates",
     "No hay actualizaciones",
     "Aucune mise \u00E0 jour"},
    /* STR_ST_ALL_CURRENT*/
    {"Your apps are up to date",
     "Tus apps est\u00E1n al d\u00EDa",
     "Vos apps sont \u00E0 jour"},
    /* STR_ST_NOTHING   */
    {"Nothing here yet",
     "Aqu\u00ED no hay nada todav\u00EDa",
     "Rien ici pour l'instant"},
    /* STR_ST_SOON      */
    {"Come back soon",
     "Vuelve pronto",
     "Revenez bient\u00F4t"},
    /* STR_ST_NO_RESULTS*/
    {"No results",
     "Sin resultados",
     "Aucun r\u00E9sultat"},
    /* STR_ST_RELOADED  */
    {"Catalogue reloaded",
     "Cat\u00E1logo recargado",
     "Catalogue recharg\u00E9"},
    /* STR_ST_CHECK     */
    {"Check for updates",
     "Buscar actualizaciones",
     "V\u00E9rifier les mises \u00E0 jour"},
    /* STR_ST_RELOAD    */
    {"Reload catalogue",
     "Recargar el cat\u00E1logo",
     "Recharger le catalogue"},
    /* STR_ST_ABOUT     */
    {"About aShop",
     "Acerca de aShop",
     "\u00C0 propos d'aShop"},
    /* STR_ST_SEARCH_PREFIX*/ {"Search: ", "B\u00FAsqueda: ", "Recherche : "},
    /* STR_ST_UPDATES_TITLE*/
    {"Download updates",
     "Descargar actualizaciones",
     "T\u00E9l\u00E9charger les mises \u00E0 jour"},
    /* STR_ST_AB_INTRO  */
    {"aShop is where apps for Aurora are found and downloaded.",
     "aShop es donde se encuentran y descargan las apps para Aurora.",
     "aShop est l'endroit o\u00F9 trouver et t\u00E9l\u00E9charger des apps "
     "pour Aurora."},
    /* STR_ST_AB_CAT    */ {"Catalogue: ", "Cat\u00E1logo: ", "Catalogue : "},
    /* STR_ST_AB_WITH   */ {", with ", ", con ", ", avec "},
    /* STR_ST_AB_APP    */ {" app and ", " app y ", " app et "},
    /* STR_ST_AB_APPS   */ {" apps and ", " apps y ", " apps et "},
    /* STR_ST_AB_NEWS1  */ {" news item.", " noticia.", " actualit\u00E9."},
    /* STR_ST_AB_NEWSN  */ {" news items.", " noticias.", " actualit\u00E9s."},
    /* STR_ST_E_NET     */
    {"The connection was lost",
     "Se perdi\u00F3 la conexi\u00F3n",
     "La connexion a \u00E9t\u00E9 perdue"},
    /* STR_ST_E_DAMAGED */
    {"The download was damaged",
     "La descarga est\u00E1 da\u00F1ada",
     "Le t\u00E9l\u00E9chargement est endommag\u00E9"},
    /* STR_ST_E_SERVER  */
    {"aShop did not send the app",
     "aShop no envi\u00F3 la app",
     "aShop n'a pas envoy\u00E9 l'app"},
    /* STR_ST_FROM_SERVER*/
    {"Up to date with aShop",
     "Al d\u00EDa con aShop",
     "\u00C0 jour avec aShop"},
    /* STR_ST_FROM_CARD */
    {"Offline: the copy saved on this console",
     "Sin conexi\u00F3n: la copia guardada en la consola",
     "Hors ligne : la copie enregistr\u00E9e sur la console"},
    /* STR_ST_AB_ONLINE */ {"from aShop", "de aShop", "depuis aShop"},
    /* STR_ST_AB_OFFLINE*/
    {"the copy saved on this console",
     "la copia guardada en la consola",
     "la copie enregistr\u00E9e sur la console"},
    /* STR_ST_AB_HOW    */
    {"Apps come from the Aurora Network over Wi-Fi. Each download is "
     "checked against the catalogue before it goes to /Aurora/Apps, "
     "where the Home Menu shows it. To publish an app, go to "
     "account.aurora3ds.xyz/developer.",
     "Las apps llegan desde Aurora Network por Wi-Fi. Cada descarga se "
     "comprueba con el cat\u00E1logo antes de ir a /Aurora/Apps, donde "
     "aparece en el men\u00FA de inicio. Para publicar una app, ve a "
     "account.aurora3ds.xyz/developer.",
     "Les apps viennent d'Aurora Network par Wi-Fi. Chaque "
     "t\u00E9l\u00E9chargement est v\u00E9rifi\u00E9 avec le catalogue "
     "avant d'aller dans /Aurora/Apps, o\u00F9 le menu d'accueil "
     "l'affiche. Pour publier une app, allez sur "
     "account.aurora3ds.xyz/developer."},
    /* STR_ST_OFFLINE_TAG*/
    {" (offline)",
     " (sin conexi\u00F3n)",
     " (hors ligne)"},
    /* STR_ST_NEED_TITLE*/
    {"Aurora account required",
     "Se necesita una cuenta Aurora",
     "Compte Aurora requis"},
    /* STR_ST_NEED_L1   */
    {"aShop needs an Aurora account.",
     "aShop necesita una cuenta Aurora.",
     "aShop a besoin d'un compte Aurora."},
    /* STR_ST_NEED_L2   */
    {"Link this console to yours, or",
     "Vincula esta consola a tu cuenta,",
     "Liez cette console \u00E0 votre compte,"},
    /* STR_ST_NEED_L3   */
    {"create one, to download apps.",
     "o crea una, para descargar apps.",
     "ou cr\u00E9ez-en un, pour les apps."},
    /* STR_ST_CONNECTING*/
    {"Connecting to aShop...",
     "Conectando con aShop...",
     "Connexion \u00E0 aShop..."},
    /* STR_ST_ICONS     */
    {"Getting icons",
     "Obteniendo iconos",
     "R\u00E9cup\u00E9ration des ic\u00F4nes"},
    /* STR_ST_NO_REACH  */
    {"Could not reach aShop",
     "Sin conexi\u00F3n con aShop",
     "aShop injoignable"},
    /* STR_ST_SAVED     */
    {"Showing the apps saved here",
     "Se muestran las apps guardadas",
     "Apps enregistr\u00E9es ici"},
    /* STR_AC_TITLE     */ {"Aurora Account", "Cuenta Aurora", "Compte Aurora"},
    /* STR_AC_NOT_LINKED*/ {"Not linked", "Sin vincular", "Non li\u00E9"},
    /* STR_AC_NONE_L1   */
    {"No account is linked to this console.",
     "No hay ninguna cuenta vinculada.",
     "Aucun compte n'est li\u00E9 \u00E0 cette console."},
    /* STR_AC_NONE_L2   */
    {"Link one to use online features.",
     "Vincula una para las funciones en l\u00EDnea.",
     "Liez-en un pour les fonctions en ligne."},
    /* STR_AC_NONE_L3   */
    {"No account yet? You can create one.",
     "\u00BFA\u00FAn sin cuenta? Puedes crear una.",
     "Pas encore de compte ? Cr\u00E9ez-en un."},
    /* STR_AC_LINK      */
    {"Link an account",
     "Vincular una cuenta",
     "Lier un compte"},
    /* STR_AC_CREATE    */
    {"Create an account",
     "Crear una cuenta",
     "Cr\u00E9er un compte"},
    /* STR_AC_SIGNED_IN */
    {"Signed in as ",
     "Sesi\u00F3n iniciada como ",
     "Connect\u00E9 en tant que "},
    /* STR_AC_LINKED_L2 */
    {"This console is linked to your account.",
     "Esta consola est\u00E1 vinculada a tu cuenta.",
     "Cette console est li\u00E9e \u00E0 votre compte."},
    /* STR_AC_CHECK     */
    {"Check account",
     "Comprobar la cuenta",
     "V\u00E9rifier le compte"},
    /* STR_AC_UNLINK    */
    {"Unlink this console",
     "Desvincular esta consola",
     "D\u00E9lier cette console"},
    /* STR_AC_HINT      */
    {"A: Select   B: Back",
     "A: Elegir   B: Atr\u00E1s",
     "A : Choisir   B : Retour"},
    /* STR_AC_GO_TO     */
    {"On a phone or computer, go to",
     "En un m\u00F3vil u ordenador, ve a",
     "Sur t\u00E9l\u00E9phone ou ordinateur, allez sur"},
    /* STR_AC_ENTER     */
    {"and enter this code:",
     "e introduce este c\u00F3digo:",
     "et saisissez ce code :"},
    /* STR_AC_CREATE_AT */
    {"On a phone or computer, create an account at",
     "En un m\u00F3vil u ordenador, crea una cuenta en",
     "Sur t\u00E9l\u00E9phone ou ordinateur, cr\u00E9ez un compte sur"},
    /* STR_AC_THEN_LINK */
    {"then choose Link a console and enter:",
     "luego elige Link a console e introduce:",
     "puis choisissez Link a console et saisissez :"},
    /* STR_AC_WAITING   */
    {"Waiting for you to approve it...",
     "Esperando tu aprobaci\u00F3n...",
     "En attente de votre approbation..."},
    /* STR_AC_RETRYING  */
    {"Connection problem. Trying again...",
     "Problema de conexi\u00F3n. Reintentando...",
     "Probl\u00E8me de connexion. Nouvel essai..."},
    /* STR_AC_EXPIRES   */
    {"Code expires in ",
     "El c\u00F3digo caduca en ",
     "Le code expire dans "},
    /* STR_AC_SCAN      */
    {"Or scan this with your phone's camera",
     "O escan\u00E9alo con la c\u00E1mara del m\u00F3vil",
     "Ou scannez-le avec votre t\u00E9l\u00E9phone"},
    /* STR_AC_CANCEL_HINT*/ {"B: Cancel", "B: Cancelar", "B : Annuler"},
    /* STR_AC_CONNECTING*/
    {"Connecting to Aurora...",
     "Conectando con Aurora...",
     "Connexion \u00E0 Aurora..."},
    /* STR_AC_JOINING   */
    {"Joining your Wi-Fi network...",
     "Conectando a tu red Wi-Fi...",
     "Connexion \u00E0 votre r\u00E9seau Wi-Fi..."},
    /* STR_AC_FINISHING */ {"Finishing...", "Terminando...", "Finalisation..."},
    /* STR_AC_LINK_DONE */
    {"This console is now linked.",
     "La consola ya est\u00E1 vinculada.",
     "La console est maintenant li\u00E9e."},
    /* STR_AC_DENIED    */
    {"The link was declined.",
     "Se rechaz\u00F3 la vinculaci\u00F3n.",
     "La liaison a \u00E9t\u00E9 refus\u00E9e."},
    /* STR_AC_DENIED2   */
    {"Deny was chosen on the website.",
     "Se eligi\u00F3 Deny en la web.",
     "Deny a \u00E9t\u00E9 choisi sur le site."},
    /* STR_AC_EXPIRED   */
    {"The code expired.",
     "El c\u00F3digo caduc\u00F3.",
     "Le code a expir\u00E9."},
    /* STR_AC_NO_NET    */
    {"Could not connect to Wi-Fi",
     "No se pudo conectar al Wi-Fi",
     "Connexion Wi-Fi impossible"},
    /* STR_AC_NO_SERVER */
    {"Could not reach Aurora",
     "No se pudo contactar con Aurora",
     "Aurora est injoignable"},
    /* STR_AC_TRY_LATER */
    {"Check the connection and try again.",
     "Comprueba la conexi\u00F3n e int\u00E9ntalo de nuevo.",
     "V\u00E9rifiez la connexion et r\u00E9essayez."},
    /* STR_AC_BUSY_SRV  */
    {"Too many tries. Wait a while.",
     "Demasiados intentos. Espera un poco.",
     "Trop d'essais. Patientez un peu."},
    /* STR_AC_REVOKED   */
    {"This console was unlinked.",
     "Esta consola se desvincul\u00F3.",
     "Cette console a \u00E9t\u00E9 d\u00E9li\u00E9e."},
    /* STR_AC_REVOKED2  */
    {"Link it again to sign in.",
     "Vuelve a vincularla para iniciar sesi\u00F3n.",
     "Liez-la \u00E0 nouveau pour vous connecter."},
    /* STR_AC_UNLINK_Q  */
    {"Unlink this console?",
     "\u00BFDesvincular esta consola?",
     "D\u00E9lier cette console ?"},
    /* STR_AC_UNLINK_SUB*/
    {"This console forgets the account.",
     "La consola olvidar\u00E1 la cuenta.",
     "La console oubliera le compte."},
    /* STR_AC_UNLINKED  */
    {"Console unlinked.",
     "Consola desvinculada.",
     "Console d\u00E9li\u00E9e."},
    /* STR_AC_REVOKE_WEB*/
    {"To remove it from the account too, go to",
     "Para quitarla tambi\u00E9n de la cuenta, ve a",
     "Pour la retirer aussi du compte, allez sur"},
    /* STR_AC_CHECKED   */
    {"Account checked just now.",
     "Cuenta comprobada.",
     "Compte v\u00E9rifi\u00E9."},
    /* STR_AC_SD_FAIL   */
    {"Could not write to the SD card.",
     "No se pudo escribir en la SD.",
     "\u00C9criture sur la carte SD impossible."},
    /* STR_AC_SITE      */
    {"Accounts are made and managed at",
     "Las cuentas se crean y gestionan en",
     "Les comptes se cr\u00E9ent et se g\u00E8rent sur"},
    /* STR_AC_SITE_LINKED*/
    {"Manage your account and consoles at",
     "Gestiona tu cuenta y tus consolas en",
     "G\u00E9rez votre compte et vos consoles sur"},
    /* STR_AC_E_NOLINK  */
    {"The Wi-Fi connection dropped.",
     "Se perdi\u00F3 la conexi\u00F3n Wi-Fi.",
     "La connexion Wi-Fi a \u00E9t\u00E9 perdue."},
    /* STR_AC_E_TIMEOUT */
    {"The server did not answer in time.",
     "El servidor no respondi\u00F3 a tiempo.",
     "Le serveur n'a pas r\u00E9pondu \u00E0 temps."},
    /* STR_AC_E_DNS     */
    {"The server's name was not found.",
     "No se encontr\u00F3 el servidor.",
     "Serveur introuvable."},
    /* STR_AC_E_ROUTER  */
    {"The router did not answer.",
     "El router no respondi\u00F3.",
     "Le routeur n'a pas r\u00E9pondu."},
    /* STR_AC_E_CHIP    */
    {"The Wi-Fi chip is busy.",
     "El chip Wi-Fi est\u00E1 ocupado.",
     "La puce Wi-Fi est occup\u00E9e."},
    /* STR_AC_E_REFUSED */
    {"The server refused the connection.",
     "El servidor rechaz\u00F3 la conexi\u00F3n.",
     "Le serveur a refus\u00E9 la connexion."},
    /* STR_AC_E_REPLY   */
    {"The server's reply was not understood.",
     "No se entendi\u00F3 la respuesta.",
     "R\u00E9ponse du serveur incomprise."},
    /* STR_AC_SETUP_L1  */
    {"Link an Aurora account to use online",
     "Vincula una cuenta Aurora para usar",
     "Liez un compte Aurora pour utiliser"},
    /* STR_AC_SETUP_L2  */
    {"features. You can also do it later.",
     "funciones en l\u00EDnea. Tambi\u00E9n puedes luego.",
     "les fonctions en ligne (ou plus tard)."},
    /* STR_AC_SETUP_BTN */
    {"Link or create",
     "Vincular o crear",
     "Lier ou cr\u00E9er"},
    /* STR_AC_SETUP_WIFI*/
    {"Set up Wi-Fi first to link an account.",
     "Configura el Wi-Fi para vincular una cuenta.",
     "Configurez le Wi-Fi pour lier un compte."},
    /* STR_AC_PLEASE_WAIT*/ {"Please wait", "Espera un momento", "Veuillez patienter"},
};

const char *L(StringId id) {
  int l = g_lang;
  if (l < 0 || l >= LANG_COUNT)
    l = 0;
  if ((unsigned)id >= (unsigned)STR_COUNT)
    return "";
  return T[id][l];
}

/* Index 0, Aurora teal, is the default. */
const Color aurora_accent_presets[AURORA_ACCENT_COUNT] = {
    {0x64, 0xE8, 0xC8},
    {0xFF, 0x3B, 0x30},
    {0xFF, 0x9F, 0x0A},
    {0xFF, 0xD6, 0x0A},
    {0x34, 0xC8, 0x3A},
    {0x1E, 0x8A, 0x3B},
    {0x32, 0xD6, 0xE2},
    {0x3B, 0x82, 0xF6},
    {0x28, 0x2F, 0xE6},
    {0xA0, 0x5C, 0xE2},
    {0xFF, 0x2D, 0xB8},
    {0xFF, 0x2D, 0x55},
    {0xB0, 0xB8, 0xE8},
    {0x9A, 0x9A, 0x9A},
};
const char *aurora_accent_names[AURORA_ACCENT_COUNT] = {
    "Aurora", "Red",    "Orange",  "Yellow", "Green",      "Forest", "Cyan",
    "Blue",   "Indigo", "Purple",  "Magenta", "Rose",      "Periwinkle", "Gray",
};

static u32 slen(const char *s) {
  u32 n = 0;
  while (*s++)
    n++;
  return n;
}

static void uint_str(u32 v, char *out) {
  char tmp[12];
  int i = 0;
  if (v == 0)
    tmp[i++] = '0';
  while (v) {
    tmp[i++] = (char)('0' + (v % 10));
    v /= 10;
  }
  int p = 0;
  while (i)
    out[p++] = tmp[--i];
  out[p] = '\0';
}

/* The layout was made for an 8px bitmap line; this centres a pack-font line on
 * the midline that line had at `y8`. */
static int mid8(int y8, const Font *f) { return y8 + FONT_HEIGHT / 2 - ui_th(f) / 2; }

/* Glyphs only, no background box, over the gradient and glow. The bitmap loop
 * is the fallback without the pack. */
static void text_tr(volatile u8 *fb, int x, int y, int sh, const char *s,
                    Color fg) {
  if (ui_have(&ui_font)) {
    ui_text(fb, x, mid8(y, &ui_font), sh, s, fg, COLOR_HM_BG, &ui_font);
    return;
  }
  int cx = x;
  while (*s) {
    char c = *s++;
    if (c < 0x20 || c > 0x7E) {
      cx += FONT_WIDTH;
      continue;
    }
    const uint8_t *g = font_data[c - 0x20];
    for (int row = 0; row < FONT_HEIGHT; row++) {
      uint8_t bits = g[row];
      for (int col = 0; col < FONT_WIDTH; col++)
        if (bits & (0x80 >> col))
          draw_pixel(fb, cx + col, y + row, sh, fg);
    }
    cx += FONT_WIDTH;
  }
}

static void text_center_tr(volatile u8 *fb, int y, int screen_w, int sh,
                           const char *s, Color fg) {
  text_tr(fb, (screen_w - ui_tw(&ui_font, s)) / 2, y, sh, s, fg);
}

static void disc(volatile u8 *fb, int cx, int cy, int r, int sh, Color c) {
  draw_filled_round_rect(fb, cx - r, cy - r, 2 * r, 2 * r, r, sh, c);
}

static void thick_line(volatile u8 *fb, int x0, int y0, int x1, int y1, int t,
                       int sh, Color c) {
  int dx = x1 - x0, dy = y1 - y0;
  int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
  int steps = adx > ady ? adx : ady;
  if (steps < 1)
    steps = 1;
  for (int i = 0; i <= steps; i++) {
    int x = x0 + dx * i / steps;
    int y = y0 + dy * i / steps;
    draw_filled_rect(fb, x, y, t, t, sh, c);
  }
}

#define SH_TOP TOP_SCREEN_HEIGHT

static void status_bar(void) { status_bar_draw(); }

static void step_icon(int step, int cx, int cy, Color col) {
  volatile u8 *fb = VRAM_TOP_LA;
  switch (step) {
    case 0: /* Language: globe (ring + crosshair) */
      disc(fb, cx, cy, 9, SH_TOP, col);
      disc(fb, cx, cy, 6, SH_TOP, COLOR_HM_BG);
      draw_filled_rect(fb, cx - 1, cy - 9, 2, 18, SH_TOP, col);
      draw_filled_rect(fb, cx - 9, cy - 1, 18, 2, SH_TOP, col);
      break;
    case 1: /* Network: three rising signal bars */
      for (int b = 0; b < 3; b++) {
        int h = 4 + b * 4;
        draw_filled_rect(fb, cx - 9 + b * 6, cy + 6 - h, 4, h, SH_TOP, col);
      }
      break;
    case 2: /* User Details: person */
      disc(fb, cx, cy - 5, 4, SH_TOP, col);
      draw_filled_round_rect(fb, cx - 7, cy + 1, 14, 9, 6, SH_TOP, col);
      break;
    case 3: /* Account: two chain links */
      draw_round_ring(fb, cx - 11, cy - 5, 13, 10, 5, 2, SH_TOP, col);
      draw_round_ring(fb, cx - 2, cy - 5, 13, 10, 5, 2, SH_TOP, col);
      break;
    case 4: /* Personalise: gear (ring + 4 teeth) */
      disc(fb, cx, cy, 8, SH_TOP, col);
      disc(fb, cx, cy, 4, SH_TOP, COLOR_HM_BG);
      draw_filled_rect(fb, cx - 2, cy - 11, 4, 4, SH_TOP, col);
      draw_filled_rect(fb, cx - 2, cy + 7, 4, 4, SH_TOP, col);
      draw_filled_rect(fb, cx - 11, cy - 2, 4, 4, SH_TOP, col);
      draw_filled_rect(fb, cx + 7, cy - 2, 4, 4, SH_TOP, col);
      break;
    case 5: /* Welcome: check mark */
      thick_line(fb, cx - 7, cy, cx - 2, cy + 5, 3, SH_TOP, col);
      thick_line(fb, cx - 2, cy + 5, cx + 8, cy - 7, 3, SH_TOP, col);
      break;
    default:
      break;
  }
}

/* Labels STR_LANGUAGE..STR_WELCOME must stay consecutive. */
static void step_bar(int active, Color accent) {
  static const int cxs[6] = {34, 100, 167, 234, 300, 366};
  for (int i = 0; i < 6; i++) {
    Color col =
        (i == active) ? accent : (i < active ? COLOR_WHITE : COLOR_HM_TEXT2);
    const char *lab = L((StringId)(STR_LANGUAGE + i));
    step_icon(i, cxs[i], 166, col);
    ui_text(VRAM_TOP_LA, cxs[i] - ui_tw(&ui_small, lab) / 2,
            mid8(194, &ui_small), SH_TOP, lab, col, COLOR_HM_BG, &ui_small);
  }
}

static void setup_top(int step, const char *l1, const char *l2, Color accent) {
  clear_screen(VRAM_TOP_LA, TOP_FB_SIZE, COLOR_HM_BG);
  status_bar();
  if (l1)
    text_center_tr(VRAM_TOP_LA, 78, TOP_SCREEN_WIDTH, SH_TOP, l1, COLOR_WHITE);
  if (l2)
    text_center_tr(VRAM_TOP_LA, 94, TOP_SCREEN_WIDTH, SH_TOP, l2, COLOR_WHITE);
  step_bar(step, accent);
}

#define SH_BOT BOT_SCREEN_HEIGHT

/* Rounded-rect corners blend with what is behind them, so repeated in-place
 * redraws would harden them. The background is flat here, so clearing the
 * corner boxes first keeps a redraw identical to a fresh one. */
static void patch_corners(int x, int y, int w, int h, int r) {
  draw_filled_rect(VRAM_BOT_A, x, y, r, r, SH_BOT, COLOR_HM_BG);
  draw_filled_rect(VRAM_BOT_A, x + w - r, y, r, r, SH_BOT, COLOR_HM_BG);
  draw_filled_rect(VRAM_BOT_A, x, y + h - r, r, r, SH_BOT, COLOR_HM_BG);
  draw_filled_rect(VRAM_BOT_A, x + w - r, y + h - r, r, r, SH_BOT, COLOR_HM_BG);
}

static void button(int x, int y, int w, int h, const char *label, int sel,
                   Color accent) {
  patch_corners(x - 2, y - 2, w + 4, h + 4, 10);
  draw_filled_round_rect(VRAM_BOT_A, x - 2, y - 2, w + 4, h + 4, 10, SH_BOT,
                         sel ? accent : COLOR_HM_BG);
  draw_filled_round_rect(VRAM_BOT_A, x, y, w, h, 8, SH_BOT, COLOR_HM_SLOT);
  ui_text(VRAM_BOT_A, x + (w - ui_tw(&ui_bold, label)) / 2,
          y + (h - ui_th(&ui_bold)) / 2, SH_BOT, label, COLOR_WHITE,
          COLOR_HM_SLOT, &ui_bold);
}

static void bottom_title(const char *s) {
  ui_text(VRAM_BOT_A, 12, mid8(12, &ui_small), SH_BOT, s, COLOR_HM_TEXT2,
          COLOR_HM_BG, &ui_small);
}

static void glow_top(void) {
  clear_screen(VRAM_TOP_LA, TOP_FB_SIZE, COLOR_HM_BG);
  int cx = TOP_SCREEN_WIDTH / 2, cy = 95;
  for (int y = 22; y < 185; y++) {
    for (int x = 0; x < TOP_SCREEN_WIDTH; x++) {
      int dx = x - cx, dy = (y - cy) * 2;
      int d2 = dx * dx + dy * dy;
      int inten = 120 - d2 / 70;
      if (inten <= 0)
        continue;
      if (inten > 120)
        inten = 120;
      Color c = {(u8)(0x16 + inten * 0x0E / 120),
                 (u8)(0x16 + inten * 0x5E / 120),
                 (u8)(0x16 + inten * 0x46 / 120)};
      draw_pixel(VRAM_TOP_LA, x, y, SH_TOP, c);
    }
  }
  status_bar();
  text_center_tr(VRAM_TOP_LA, 56, TOP_SCREEN_WIDTH, SH_TOP, "Welcome to",
                 COLOR_WHITE);
  int lx = (TOP_SCREEN_WIDTH - AURORA_LOGO_WIDTH) / 2;
  draw_aurora_logo(VRAM_TOP_LA, lx, 78, SH_TOP, COLOR_WHITE);
  ui_text(VRAM_TOP_LA, 12, mid8(214, &ui_small), SH_TOP, AURORA_VERSION,
          COLOR_HM_TEXT2, COLOR_HM_BG, &ui_small);
}

static const char *lang_names[LANG_COUNT] = {"English", "Espa\u00F1ol",
                                              "Fran\u00E7ais"};

#define LANG_RX   12
#define LANG_RW   (BOT_SCREEN_WIDTH - 24)
#define LANG_RH   40
#define LANG_Y0   34
#define LANG_STEP 48

/* The rect is opaque and a fixed size, so redrawing a row erases its last
 * state. */
static void lang_row(int i, int sel, Color accent) {
  int y = LANG_Y0 + i * LANG_STEP;
  int on = (i == sel);
  patch_corners(LANG_RX, y, LANG_RW, LANG_RH, 8);
  draw_filled_round_rect(VRAM_BOT_A, LANG_RX, y, LANG_RW, LANG_RH, 8, SH_BOT,
                         on ? accent : COLOR_HM_SLOT);
  ui_text(VRAM_BOT_A, LANG_RX + 16, y + (LANG_RH - ui_th(&ui_font)) / 2, SH_BOT,
          lang_names[i], COLOR_WHITE, on ? accent : COLOR_HM_SLOT, &ui_font);
  if (on) {
    thick_line(VRAM_BOT_A, LANG_RX + LANG_RW - 30, y + LANG_RH / 2,
               LANG_RX + LANG_RW - 24, y + LANG_RH / 2 + 6, 2, SH_BOT,
               COLOR_WHITE);
    thick_line(VRAM_BOT_A, LANG_RX + LANG_RW - 24, y + LANG_RH / 2 + 6,
               LANG_RX + LANG_RW - 14, y + LANG_RH / 2 - 6, 2, SH_BOT,
               COLOR_WHITE);
  }
}

/* The caption is in the language being chosen, so it is redrawn with the rows. */
static void lang_button(Color accent) {
  button((BOT_SCREEN_WIDTH - 200) / 2, BOT_SCREEN_HEIGHT - 38, 200, 30,
         L(STR_GET_STARTED), 1, accent);
}

static void lang_bottom(int sel, Color accent) {
  clear_screen(VRAM_BOT_A, BOT_FB_SIZE, COLOR_HM_BG);
  bottom_title("Home Menu");
  for (int i = 0; i < LANG_COUNT; i++)
    lang_row(i, sel, accent);
  lang_button(accent);
  screen_present_bottom();
}

static void lang_update(int old_sel, int sel, Color accent) {
  lang_row(old_sel, sel, accent);
  lang_row(sel, sel, accent);
  lang_button(accent);
  screen_present_bottom();
}

static void step_language(UserConfig *cfg) {
  Color accent = aurora_accent_presets[cfg->accent];
  g_lang = cfg->language;
  glow_top();
  screen_present_top();
  int sel = cfg->language;
  lang_bottom(sel, accent);
  while (1) {
    u32 k = get_keys_down();
    int prev = sel;
    if ((k & BUTTON_DUP) && sel > 0)
      sel--;
    if ((k & BUTTON_DDOWN) && sel < LANG_COUNT - 1)
      sel++;

    int go = 0, tx, ty;
    if (touch_tap(&tx, &ty)) {
      int rx = 12, rw = BOT_SCREEN_WIDTH - 24, rh = 40, y0 = 34, step = 48;
      for (int i = 0; i < LANG_COUNT; i++)
        if (touch_in(tx, ty, rx, y0 + i * step, rw, rh))
          sel = i;
      if (touch_in(tx, ty, (BOT_SCREEN_WIDTH - 200) / 2,
                   BOT_SCREEN_HEIGHT - 38, 200, 30))
        go = 1;
    }

    if (sel != prev) {
      cfg->language = (u8)sel;
      g_lang = sel;
      lang_update(prev, sel, accent);
    }
    if ((k & (BUTTON_A | BUTTON_START)) || go) {
      cfg->language = (u8)sel;
      return;
    }
    ui_idle();
  }
}

typedef enum { NAV_NEXT, NAV_BACK } Nav;

static const SetupWifi *s_wifi;

#define NET_BX 60
#define NET_BW 200
#define NET_BH 40
#define NET_BY(i) (86 + (i) * 56)

static void net_buttons(int sel, const char *saved, Color accent) {
  button(NET_BX, NET_BY(0), NET_BW, NET_BH, L(STR_NET_SETUP), sel == 0,
         accent);
  button(NET_BX, NET_BY(1), NET_BW, NET_BH,
         L(saved[0] ? STR_NEXT : STR_SKIP), sel == 1, accent);
  screen_present_bottom();
}

/* Set up Wi-Fi opens Settings > Wi-Fi itself, which first offers to copy the
 * firmware when the card lacks it. */
static void net_draw(int sel, const char *saved, Color accent) {
  char line[80];
  setup_top(1, L(STR_NET_L1), L(STR_NET_L2), accent);
  screen_present_top();
  clear_screen(VRAM_BOT_A, BOT_FB_SIZE, COLOR_HM_BG);
  bottom_title("Home Menu");
  if (saved[0]) {
    char *p = line, *e = line + sizeof(line) - 1;
    for (const char *q = L(STR_NET_SAVED); *q && p < e; q++)
      *p++ = *q;
    for (const char *q = saved; *q && p < e; q++)
      *p++ = *q;
    *p = '\0';
  }
  text_center_tr(VRAM_BOT_A, 50, BOT_SCREEN_WIDTH, SH_BOT,
                 saved[0] ? line
                 : fwdump_present() ? L(STR_NET_READY) : L(STR_NET_FW),
                 COLOR_HM_TEXT2);
  text_center_tr(VRAM_BOT_A, BOT_SCREEN_HEIGHT - 22, BOT_SCREEN_WIDTH, SH_BOT,
                 L(STR_NET_HINT), COLOR_HM_TEXT2);
  net_buttons(sel, saved, accent);
}

static Nav step_network(UserConfig *cfg) {
  Color accent = aurora_accent_presets[cfg->accent];
  const char *saved = s_wifi ? s_wifi->wifi_saved() : "";
  int sel = 0;

  net_draw(sel, saved, accent);
  while (1) {
    u32 k = get_keys_down();
    int prev = sel, go = 0, tx, ty;
    if (k & BUTTON_DUP)
      sel = 0;
    if (k & BUTTON_DDOWN)
      sel = 1;
    if (touch_tap(&tx, &ty))
      for (int i = 0; i < 2; i++)
        if (touch_in(tx, ty, NET_BX, NET_BY(i), NET_BW, NET_BH)) {
          sel = i;
          go = 1;
        }
    if (k & BUTTON_A)
      go = 1;
    if (go && sel == 0 && s_wifi) {
      anim_transition(ANIM_PUSH, ANIM_BOTH);
      s_wifi->wifi_screen();
      anim_transition(ANIM_POP, ANIM_BOTH);
      saved = s_wifi->wifi_saved();
      sel = 1;
      net_draw(sel, saved, accent);
      continue;
    }
    if ((go && sel == 1) || (k & BUTTON_START))
      return NAV_NEXT;
    if (k & BUTTON_B)
      return NAV_BACK;
    if (sel != prev)
      net_buttons(sel, saved, accent);
    ui_idle();
  }
}

static const char *kb_rows[4] = {
    "1234567890",
    "qwertyuiop",
    "asdfghjkl",
    "zxcvbnm",
};
static const char *kb_special[4] = {"Caps", "Space", "Del", "OK"};

/* KB_PASSWORD's third Caps state: with the letters and digits, all of
 * printable ASCII. */
static const char *kb_sym_rows[4] = {
    "!@#$%^&*()",
    "-_=+[]{}\\|",
    ";:'\",.<>/?",
    "`~",
};

/* KB_FILENAME and KB_PASSWORD add the punctuation file names need to the last
 * row. */
static int kb_mode;
static const char *kb_hint;
static int kb_layer; /* the caps state the rows are drawn for */

static const char *kb_row(int row) {
  if (kb_mode == KB_PASSWORD && kb_layer == 2)
    return kb_sym_rows[row];
  return (row == 3 && kb_mode != KB_NAME) ? "zxcvbnm-_." : kb_rows[row];
}

static int kb_row_len(int row) {
  if (row < 4)
    return (int)slen(kb_row(row));
  return 4; /* action row */
}

static char kb_apply_caps(char c, int caps) {
  if (caps == 1 && c >= 'a' && c <= 'z')
    return (char)(c - 'a' + 'A');
  return c;
}

#define KB_KW    27
#define KB_KH    26
#define KB_GAP   3
#define KB_Y0    52
#define KB_YSTEP (KB_KH + KB_GAP)

/* Redrawn whole, so a deleted character's cell is cleared. Text too long for
 * the field shows its end, where typing happens. */
static void kb_field(const char *name) {
  const int avail = BOT_SCREEN_WIDTH - 40;
  draw_filled_round_rect(VRAM_BOT_A, 12, 10, BOT_SCREEN_WIDTH - 24, 30, 8,
                         SH_BOT, COLOR_HM_SLOT);
  if (name[0]) {
    while (name[1] && ui_tw(&ui_font, name) > avail) {
      name++;
      while ((*name & 0xC0) == 0x80)
        name++;
    }
    ui_text(VRAM_BOT_A, 20, 10 + (30 - ui_th(&ui_font)) / 2, SH_BOT, name,
            COLOR_WHITE, COLOR_HM_SLOT, &ui_font);
  } else {
    ui_text(VRAM_BOT_A, 20, 10 + (30 - ui_th(&ui_font)) / 2, SH_BOT,
            kb_hint ? kb_hint : L(STR_KB_ENTER_NAME), COLOR_HM_TEXT2,
            COLOR_HM_SLOT, &ui_font);
  }
}

/* One key. Rows 0..3 are letters, row 4 is Caps / Space / Del / OK. Every key
 * paints an opaque face of a fixed size, so a key can be repainted alone. */
static void kb_key(int r, int c, int row, int col, int caps, Color accent) {
  int on = (r == row && c == col);
  if (r < 4) {
    int n = kb_row_len(r);
    int roww = n * KB_KW + (n - 1) * KB_GAP;
    int x = (BOT_SCREEN_WIDTH - roww) / 2 + c * (KB_KW + KB_GAP);
    int y = KB_Y0 + r * KB_YSTEP;
    char ch[2] = {kb_apply_caps(kb_row(r)[c], caps), 0};
    patch_corners(x, y, KB_KW, KB_KH, 5);
    draw_filled_round_rect(VRAM_BOT_A, x, y, KB_KW, KB_KH, 5, SH_BOT,
                           on ? accent : COLOR_HM_SLOT);
    ui_text(VRAM_BOT_A, x + (KB_KW - ui_tw(&ui_font, ch)) / 2,
            y + (KB_KH - ui_th(&ui_font)) / 2, SH_BOT, ch, COLOR_WHITE,
            on ? accent : COLOR_HM_SLOT, &ui_font);
    return;
  }

  static const int aw[4] = {56, 96, 56, 56};
  static const char *layers[3] = {"abc", "ABC", "#+="};
  int ay = KB_Y0 + 4 * KB_YSTEP, ax = 20;
  for (int i = 0; i < c; i++)
    ax += aw[i] + 8;
  Color face = (c == 0 && caps) ? accent : COLOR_HM_SLOT;
  const char *label = (c == 0 && kb_mode == KB_PASSWORD) ? layers[caps]
                                                         : kb_special[c];
  patch_corners(ax - 2, ay - 2, aw[c] + 4, KB_KH + 4, 6);
  draw_filled_round_rect(VRAM_BOT_A, ax - 2, ay - 2, aw[c] + 4, KB_KH + 4, 6,
                         SH_BOT, on ? accent : COLOR_HM_BG);
  draw_filled_round_rect(VRAM_BOT_A, ax, ay, aw[c], KB_KH, 5, SH_BOT, face);
  ui_text(VRAM_BOT_A, ax + (aw[c] - ui_tw(&ui_small, label)) / 2,
          ay + (KB_KH - ui_th(&ui_small)) / 2, SH_BOT, label, COLOR_WHITE,
          face, &ui_small);
}

static void kb_all_keys(int row, int col, int caps, Color accent) {
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < kb_row_len(r); c++)
      kb_key(r, c, row, col, caps, accent);
  for (int c = 0; c < 4; c++)
    kb_key(4, c, row, col, caps, accent);
}

static void kb_draw(const char *name, int row, int col, int caps,
                    Color accent) {
  clear_screen(VRAM_BOT_A, BOT_FB_SIZE, COLOR_HM_BG);
  kb_field(name);
  kb_all_keys(row, col, caps, accent);
  screen_present_bottom();
}

/* Moving the cursor touches two keys. A caps change relabels every letter, and
 * a press can change the name, so those redraw more. */
static void kb_update(const char *name, int orow, int ocol, int row, int col,
                      int caps, int caps_changed, int name_changed,
                      Color accent) {
  if (name_changed)
    kb_field(name);
  if (caps_changed) {
    /* The symbol layer's rows are another length: clear the old keys. */
    if (kb_mode == KB_PASSWORD)
      draw_filled_rect(VRAM_BOT_A, 0, KB_Y0 - 2, BOT_SCREEN_WIDTH,
                       4 * KB_YSTEP + 2, SH_BOT, COLOR_HM_BG);
    kb_all_keys(row, col, caps, accent);
  } else {
    kb_key(orow, ocol, row, col, caps, accent);
    kb_key(row, col, row, col, caps, accent);
  }
  screen_present_bottom();
}

/* Drops the last character, all of its bytes if it is multi-byte UTF-8. */
static void kb_backspace(char *name, int *len) {
  while (*len > 0 && ((u8)name[*len - 1] & 0xC0u) == 0x80u)
    name[--*len] = '\0';
  if (*len > 0)
    name[--*len] = '\0';
}

int keyboard_edit(char *name, int size, int mode, const char *hint,
                  Color accent) {
  int row = 1, col = 0, caps = 0;
  int len = (int)slen(name);
  int states = mode == KB_PASSWORD ? 3 : 2;
  kb_mode = mode;
  kb_hint = hint;
  kb_layer = 0;
  kb_draw(name, row, col, caps, accent);
  while (1) {
    u32 k = get_keys_down();
    int changed = 0;
    int orow = row, ocol = col, ocaps = caps, olen = len;

    if (k & BUTTON_DUP) {
      row = (row > 0) ? row - 1 : 4;
      changed = 1;
    }
    if (k & BUTTON_DDOWN) {
      row = (row < 4) ? row + 1 : 0;
      changed = 1;
    }
    if (changed) {
      int n = kb_row_len(row);
      if (col >= n)
        col = n - 1;
    }
    if (k & BUTTON_DLEFT) {
      col = (col > 0) ? col - 1 : kb_row_len(row) - 1;
      changed = 1;
    }
    if (k & BUTTON_DRIGHT) {
      col = (col < kb_row_len(row) - 1) ? col + 1 : 0;
      changed = 1;
    }

    int commit = 0; /* pressed a key (A or touch) */
    char typed = 0;
    int press = (k & BUTTON_A) ? 1 : 0;

    /* Hit-test the keys; the layout mirrors kb_draw. */
    {
      int tx, ty;
      if (touch_tap(&tx, &ty)) {
        int kw = 27, kh = 26, gap = 3, y0 = 52, ystep = kh + gap, found = 0;
        for (int r = 0; r < 4 && !found; r++) {
          int n = kb_row_len(r), roww = n * kw + (n - 1) * gap;
          int x0 = (BOT_SCREEN_WIDTH - roww) / 2;
          for (int c = 0; c < n; c++) {
            if (touch_in(tx, ty, x0 + c * (kw + gap), y0 + r * ystep, kw, kh)) {
              row = r;
              col = c;
              press = 1;
              changed = 1;
              found = 1;
              break;
            }
          }
        }
        if (!found) {
          int ay = y0 + 4 * ystep, aw[4] = {56, 96, 56, 56}, ax = 20;
          for (int c = 0; c < 4; c++) {
            if (touch_in(tx, ty, ax, ay, aw[c], kh)) {
              row = 4;
              col = c;
              press = 1;
              changed = 1;
              break;
            }
            ax += aw[c] + 8;
          }
        }
      }
    }

    if (press) {
      if (row < 4) {
        typed = kb_apply_caps(kb_row(row)[col], caps);
      } else {
        switch (col) {
          case 0:
            caps = (caps + 1) % states;
            break;
          case 1:
            typed = ' ';
            break;
          case 2: /* Del */
            kb_backspace(name, &len);
            break;
          case 3: /* OK */
            return 1;
        }
      }
      commit = 1;
    }
    if (k & BUTTON_B) {
      kb_backspace(name, &len);
      commit = 1;
    }
    if (k & BUTTON_START)
      return 1;
    if (k & BUTTON_SELECT)
      return 0;
    if (k & BUTTON_L) {
      caps = (caps + 1) % states;
      commit = 1;
    }
    if (caps != ocaps) {
      kb_layer = caps;
      if (row < 4 && col >= kb_row_len(row))
        col = kb_row_len(row) - 1;
    }

    if (typed && len < size - 1) {
      name[len++] = typed;
      name[len] = '\0';
    }

    if (changed || commit)
      kb_update(name, orow, ocol, row, col, caps, caps != ocaps, len != olen,
                accent);
    ui_idle();
  }
}

/* Focus order: 0 Name, 1 Day, 2 Month, 3 Year, 4 Back, 5 Next.
 * Left/Right move focus; Up/Down adjust the focused date field. */

static void clamp_date(UserConfig *cfg) {
  if (cfg->birth_day < 1)
    cfg->birth_day = 1;
  if (cfg->birth_day > 31)
    cfg->birth_day = 31;
  if (cfg->birth_month < 1)
    cfg->birth_month = 1;
  if (cfg->birth_month > 12)
    cfg->birth_month = 12;
  if (cfg->birth_year < 1900)
    cfg->birth_year = 1900;
  if (cfg->birth_year > 2025)
    cfg->birth_year = 2025;
}

static void date_box(int x, int y, const char *label, u32 val, int digits,
                     int sel, Color accent) {
  int w = (digits == 4) ? 68 : 52, h = 40;
  /* The label is antialiased and redrawn in place, so its strip is cleared
   * first. */
  draw_filled_rect(VRAM_BOT_A, x - 2, y - 20, w + 4, 18, SH_BOT, COLOR_HM_BG);
  ui_text(VRAM_BOT_A, x, y - 19, SH_BOT, label, COLOR_HM_TEXT2, COLOR_HM_BG,
          &ui_small);
  patch_corners(x - 2, y - 2, w + 4, h + 4, 8);
  draw_filled_round_rect(VRAM_BOT_A, x - 2, y - 2, w + 4, h + 4, 8, SH_BOT,
                         sel ? accent : COLOR_HM_BG);
  draw_filled_round_rect(VRAM_BOT_A, x, y, w, h, 6, SH_BOT, COLOR_HM_SLOT);
  char s[8];
  uint_str(val, s);
  ui_text(VRAM_BOT_A, x + (w - ui_tw(&ui_title, s)) / 2,
          y + (h - ui_th(&ui_title)) / 2, SH_BOT, s, COLOR_WHITE, COLOR_HM_SLOT,
          &ui_title);
}

/* Item i of the focus order above. Each paints an opaque frame of a fixed
 * size, so it can be repainted alone. */
static void user_item(const UserConfig *cfg, int i, int focus, Color accent) {
  switch (i) {
    case 0:
      patch_corners(12, 30, BOT_SCREEN_WIDTH - 24, 34, 8);
      draw_filled_round_rect(VRAM_BOT_A, 12, 30, BOT_SCREEN_WIDTH - 24, 34, 8,
                             SH_BOT, (focus == 0) ? accent : COLOR_HM_BG);
      draw_filled_round_rect(VRAM_BOT_A, 15, 33, BOT_SCREEN_WIDTH - 30, 28, 6,
                             SH_BOT, COLOR_HM_SLOT);
      if (cfg->name[0])
        ui_text(VRAM_BOT_A, 24, 33 + (28 - ui_th(&ui_font)) / 2, SH_BOT,
                cfg->name, COLOR_WHITE, COLOR_HM_SLOT, &ui_font);
      else
        ui_text(VRAM_BOT_A, 24, 33 + (28 - ui_th(&ui_font)) / 2, SH_BOT,
                L(STR_USER_NAME), COLOR_HM_TEXT2, COLOR_HM_SLOT, &ui_font);
      break;
    case 1:
      date_box(40, 96, L(STR_DAY), cfg->birth_day, 2, focus == 1, accent);
      break;
    case 2:
      date_box(120, 96, L(STR_MONTH), cfg->birth_month, 2, focus == 2, accent);
      break;
    case 3:
      date_box(200, 96, L(STR_YEAR), cfg->birth_year, 4, focus == 3, accent);
      break;
    case 4:
      button(40, 168, 100, 32, L(STR_BACK), focus == 4, accent);
      break;
    default:
      button(180, 168, 100, 32, L(STR_NEXT), focus == 5, accent);
      break;
  }
}

static void user_bottom(const UserConfig *cfg, int focus, Color accent) {
  clear_screen(VRAM_BOT_A, BOT_FB_SIZE, COLOR_HM_BG);
  bottom_title("Home Menu");
  for (int i = 0; i < 6; i++)
    user_item(cfg, i, focus, accent);
  text_center_tr(VRAM_BOT_A, BOT_SCREEN_HEIGHT - 16, BOT_SCREEN_WIDTH, SH_BOT,
                 L(STR_USER_HINT), COLOR_HM_TEXT2);
  screen_present_bottom();
}

static void user_update(const UserConfig *cfg, int old_focus, int focus,
                        Color accent) {
  user_item(cfg, old_focus, focus, accent);
  if (focus != old_focus)
    user_item(cfg, focus, focus, accent);
  screen_present_bottom();
}

static Nav step_user(UserConfig *cfg) {
  Color accent = aurora_accent_presets[cfg->accent];
  setup_top(2, L(STR_USER_L1), L(STR_USER_L2), accent);
  screen_present_top();

  clamp_date(cfg);
  int focus = 0;
  user_bottom(cfg, focus, accent);
  while (1) {
    u32 k = get_keys_down();
    int redraw = 0, full = 0, old_focus = focus;

    if (k & BUTTON_DLEFT) {
      focus = (focus > 0) ? focus - 1 : 5;
      redraw = 1;
    }
    if (k & BUTTON_DRIGHT) {
      focus = (focus < 5) ? focus + 1 : 0;
      redraw = 1;
    }
    if (k & (BUTTON_DUP | BUTTON_DDOWN)) {
      int d = (k & BUTTON_DUP) ? 1 : -1;
      if (focus == 1)
        cfg->birth_day = (u8)(cfg->birth_day + d);
      else if (focus == 2)
        cfg->birth_month = (u8)(cfg->birth_month + d);
      else if (focus == 3)
        cfg->birth_year = (u16)(cfg->birth_year + d);
      clamp_date(cfg);
      redraw = 1;
    }
    if (k & BUTTON_A) {
      if (focus == 0) {
        anim_transition(ANIM_PUSH, ANIM_BOT);
        keyboard_edit(cfg->name, USER_NAME_MAX, KB_NAME, NULL, accent);
        anim_transition(ANIM_POP, ANIM_BOT);
        setup_top(2, L(STR_USER_L1), L(STR_USER_L2), accent);
        screen_present_top();
        redraw = full = 1;
      } else if (focus == 4) {
        return NAV_BACK;
      } else if (focus == 5) {
        return NAV_NEXT;
      }
    }

    /* Touch: name field -> keyboard; tap a date box's upper half to +1, lower
     * half to -1; Back / Next buttons. */
    int tx, ty;
    if (touch_tap(&tx, &ty)) {
      if (touch_in(tx, ty, 12, 30, BOT_SCREEN_WIDTH - 24, 34)) {
        anim_transition(ANIM_PUSH, ANIM_BOT);
        keyboard_edit(cfg->name, USER_NAME_MAX, KB_NAME, NULL, accent);
        anim_transition(ANIM_POP, ANIM_BOT);
        setup_top(2, L(STR_USER_L1), L(STR_USER_L2), accent);
        screen_present_top();
        redraw = full = 1;
      } else if (touch_in(tx, ty, 40, 96, 52, 40)) {
        cfg->birth_day = (u8)(cfg->birth_day + (ty < 116 ? 1 : -1));
        clamp_date(cfg);
        redraw = 1;
      } else if (touch_in(tx, ty, 120, 96, 52, 40)) {
        cfg->birth_month = (u8)(cfg->birth_month + (ty < 116 ? 1 : -1));
        clamp_date(cfg);
        redraw = 1;
      } else if (touch_in(tx, ty, 200, 96, 68, 40)) {
        cfg->birth_year = (u16)(cfg->birth_year + (ty < 116 ? 1 : -1));
        clamp_date(cfg);
        redraw = 1;
      } else if (touch_in(tx, ty, 40, 168, 100, 32)) {
        return NAV_BACK;
      } else if (touch_in(tx, ty, 180, 168, 100, 32)) {
        return NAV_NEXT;
      }
    }

    if (k & BUTTON_B)
      return NAV_BACK;
    if (k & BUTTON_START)
      return NAV_NEXT;

    if (full)
      user_bottom(cfg, focus, accent);
    else if (redraw)
      user_update(cfg, old_focus, focus, accent);
    ui_idle();
  }
}

/* Link or create opens Settings > Aurora Account itself. It uses the network
 * the network step saved, and links under the name the details step took. */
static void acct_buttons(int sel, Color accent) {
  button(NET_BX, NET_BY(0), NET_BW, NET_BH, L(STR_AC_SETUP_BTN), sel == 0,
         accent);
  button(NET_BX, NET_BY(1), NET_BW, NET_BH,
         L(account_linked() ? STR_NEXT : STR_SKIP), sel == 1, accent);
  screen_present_bottom();
}

static void acct_draw(int sel, Color accent) {
  char line[80];
  const char *say = line;
  setup_top(3, L(STR_AC_SETUP_L1), L(STR_AC_SETUP_L2), accent);
  screen_present_top();
  clear_screen(VRAM_BOT_A, BOT_FB_SIZE, COLOR_HM_BG);
  bottom_title("Home Menu");
  if (account_linked() && account_name()[0]) {
    char *p = line, *e = line + sizeof(line) - 1;
    for (const char *q = L(STR_AC_SIGNED_IN); *q && p < e; q++)
      *p++ = *q;
    for (const char *q = account_name(); *q && p < e; q++)
      *p++ = *q;
    *p = '\0';
  } else if (account_linked()) {
    say = L(STR_AC_LINKED_L2);
  } else if (s_wifi && !s_wifi->wifi_saved()[0]) {
    say = L(STR_AC_SETUP_WIFI);
  } else {
    say = L(STR_AC_NONE_L1);
  }
  text_center_tr(VRAM_BOT_A, 50, BOT_SCREEN_WIDTH, SH_BOT, say,
                 COLOR_HM_TEXT2);
  text_center_tr(VRAM_BOT_A, BOT_SCREEN_HEIGHT - 22, BOT_SCREEN_WIDTH, SH_BOT,
                 L(STR_NET_HINT), COLOR_HM_TEXT2);
  acct_buttons(sel, accent);
}

static Nav step_account(UserConfig *cfg) {
  Color accent = aurora_accent_presets[cfg->accent];
  account_load();
  int sel = account_linked();

  acct_draw(sel, accent);
  while (1) {
    u32 k = get_keys_down();
    int prev = sel, go = 0, tx, ty;
    if (k & BUTTON_DUP)
      sel = 0;
    if (k & BUTTON_DDOWN)
      sel = 1;
    if (touch_tap(&tx, &ty))
      for (int i = 0; i < 2; i++)
        if (touch_in(tx, ty, NET_BX, NET_BY(i), NET_BW, NET_BH)) {
          sel = i;
          go = 1;
        }
    if (k & BUTTON_A)
      go = 1;
    if (go && sel == 0) {
      anim_transition(ANIM_PUSH, ANIM_BOTH);
      account_screen(cfg->name);
      anim_transition(ANIM_POP, ANIM_BOTH);
      sel = account_linked();
      acct_draw(sel, accent);
      continue;
    }
    if ((go && sel == 1) || (k & BUTTON_START))
      return NAV_NEXT;
    if (k & BUTTON_B)
      return NAV_BACK;
    if (sel != prev)
      acct_buttons(sel, accent);
    ui_idle();
  }
}

#define PSW 36
#define PGAP 10
#define PCOLS 6
#define PGW (PCOLS * PSW + (PCOLS - 1) * PGAP)
#define PX ((BOT_SCREEN_WIDTH - PGW) / 2)
#define PY 44
#define PROWS ((AURORA_ACCENT_COUNT + PCOLS - 1) / PCOLS)

/* One palette swatch. Unlike the other widgets the selection ring sits outside
 * the swatch, so a deselected cell has to have that area cleared first. */
static void accent_cell(int i, int sel) {
  int col = i % PCOLS, row = i / PCOLS;
  int x = PX + col * (PSW + PGAP), y = PY + row * (PSW + PGAP);
  if (i == sel)
    draw_filled_round_rect(VRAM_BOT_A, x - 4, y - 4, PSW + 8, PSW + 8, PSW / 2,
                           SH_BOT, COLOR_WHITE);
  else
    draw_filled_rect(VRAM_BOT_A, x - 4, y - 4, PSW + 8, PSW + 8, SH_BOT,
                     COLOR_HM_BG);
  disc(VRAM_BOT_A, x + PSW / 2, y + PSW / 2, PSW / 2, SH_BOT,
       (i == sel) ? COLOR_HM_BG : aurora_accent_presets[i]);
  disc(VRAM_BOT_A, x + PSW / 2, y + PSW / 2,
       (i == sel) ? PSW / 2 - 4 : PSW / 2, SH_BOT, aurora_accent_presets[i]);
}

/* Both buttons carry the accent, so they follow the selection. */
static void accent_buttons(Color accent) {
  button(40, 198, 100, 30, L(STR_BACK), 0, accent);
  button(180, 198, 100, 30, L(STR_NEXT), 1, accent);
}

static void accent_bottom(int sel) {
  clear_screen(VRAM_BOT_A, BOT_FB_SIZE, COLOR_HM_BG);
  bottom_title("Home Menu");
  for (int i = 0; i < AURORA_ACCENT_COUNT; i++)
    accent_cell(i, sel);
  accent_buttons(aurora_accent_presets[sel]);
  screen_present_bottom();
}

static void accent_update(int old_sel, int sel) {
  accent_cell(old_sel, sel);
  accent_cell(sel, sel);
  accent_buttons(aurora_accent_presets[sel]);
  screen_present_bottom();
}

static Nav step_personalise(UserConfig *cfg) {
  int sel = cfg->accent;
  setup_top(4, L(STR_PERS_L1), 0, aurora_accent_presets[sel]);
  screen_present_top();
  accent_bottom(sel);
  while (1) {
    u32 k = get_keys_down();
    int prev = sel;
    int col = sel % PCOLS, row = sel / PCOLS;
    if ((k & BUTTON_DLEFT) && col > 0)
      sel--;
    if ((k & BUTTON_DRIGHT) && col < PCOLS - 1 && sel + 1 < AURORA_ACCENT_COUNT)
      sel++;
    if ((k & BUTTON_DUP) && row > 0)
      sel -= PCOLS;
    if ((k & BUTTON_DDOWN) && row < PROWS - 1 &&
        sel + PCOLS < AURORA_ACCENT_COUNT)
      sel += PCOLS;
    int tx, ty;
    if (touch_tap(&tx, &ty)) {
      for (int i = 0; i < AURORA_ACCENT_COUNT; i++) {
        int c = i % PCOLS, r = i / PCOLS;
        int x = PX + c * (PSW + PGAP), y = PY + r * (PSW + PGAP);
        if (touch_in(tx, ty, x, y, PSW, PSW)) {
          sel = i;
          break;
        }
      }
      if (touch_in(tx, ty, 40, 198, 100, 30)) {
        cfg->accent = (u8)sel;
        return NAV_BACK;
      }
      if (touch_in(tx, ty, 180, 198, 100, 30)) {
        cfg->accent = (u8)sel;
        return NAV_NEXT;
      }
    }

    if (sel != prev) {
      cfg->accent = (u8)sel;
      setup_top(4, L(STR_PERS_L1), 0, aurora_accent_presets[sel]);
      screen_present_top();
      accent_update(prev, sel);
    }
    if (k & (BUTTON_A | BUTTON_START)) {
      cfg->accent = (u8)sel;
      return NAV_NEXT;
    }
    if (k & BUTTON_B)
      return NAV_BACK;
    ui_idle();
  }
}

static void console_icon(int cx, int cy, Color color) {
  volatile u8 *fb = VRAM_TOP_LA;
  draw_filled_round_rect(fb, cx - 34, cy - 44, 68, 40, 8, SH_TOP, color);
  draw_filled_round_rect(fb, cx - 26, cy - 38, 52, 26, 4, SH_TOP, COLOR_HM_BG);
  draw_filled_round_rect(fb, cx - 34, cy, 68, 44, 8, SH_TOP, color);
  draw_filled_round_rect(fb, cx - 20, cy + 6, 34, 26, 4, SH_TOP, COLOR_HM_BG);
  draw_filled_rect(fb, cx + 18, cy + 14, 3, 9, SH_TOP, COLOR_HM_BG);
  draw_filled_rect(fb, cx + 15, cy + 17, 9, 3, SH_TOP, COLOR_HM_BG);
}

static Nav step_welcome(UserConfig *cfg) {
  Color accent = aurora_accent_presets[cfg->accent];
  clear_screen(VRAM_TOP_LA, TOP_FB_SIZE, COLOR_HM_BG);
  status_bar();
  console_icon(TOP_SCREEN_WIDTH / 2, 70, COLOR_WHITE);
  step_bar(5, accent);
  screen_present_top();

  clear_screen(VRAM_BOT_A, BOT_FB_SIZE, COLOR_HM_BG);
  bottom_title("Home Menu");
  text_center_tr(VRAM_BOT_A, 100, BOT_SCREEN_WIDTH, SH_BOT, L(STR_PRESS_A_START),
                 COLOR_WHITE);
  text_center_tr(VRAM_BOT_A, BOT_SCREEN_HEIGHT - 20, BOT_SCREEN_WIDTH, SH_BOT,
                 L(STR_B_BACK), COLOR_HM_TEXT2);
  screen_present_bottom();

  while (1) {
    u32 k = get_keys_down();
    int tx, ty;
    if (touch_tap(&tx, &ty))
      return NAV_NEXT;
    if (k & (BUTTON_A | BUTTON_START))
      return NAV_NEXT;
    if (k & BUTTON_B)
      return NAV_BACK;
    ui_idle();
  }
}

void setup_run(UserConfig *cfg, const SetupWifi *wifi) {
  s_wifi = wifi;
  g_lang = cfg->language;
  int step = 0;
  while (step < 6) {
    Nav n = NAV_NEXT;
    switch (step) {
      case 0:
        step_language(cfg);
        n = NAV_NEXT;
        break;
      case 1:
        n = step_network(cfg);
        break;
      case 2:
        n = step_user(cfg);
        break;
      case 3:
        n = step_account(cfg);
        break;
      case 4:
        n = step_personalise(cfg);
        break;
      case 5:
        n = step_welcome(cfg);
        break;
    }
    if (n == NAV_BACK) {
      if (step > 0)
        step--;
      anim_transition(ANIM_POP, ANIM_BOTH);
    } else {
      step++;
      if (step < 6)
        anim_transition(ANIM_PUSH, ANIM_BOTH);
    }
  }
  cfg->setup_done = 1;
  cfg->valid = 1;
}

static FATFS s_fs;
static FIL s_fil;
static u8 s_buf[USER_DAT_SIZE];

void user_config_defaults(UserConfig *cfg) {
  cfg->valid = 0;
  cfg->setup_done = 0;
  cfg->language = LANG_ENGLISH;
  cfg->accent = 0;
  cfg->birth_day = 1;
  cfg->birth_month = 1;
  cfg->birth_year = 2000;
  for (int i = 0; i < USER_NAME_MAX; i++)
    cfg->name[i] = 0;
  cfg->touch_set = 0;
  touch_cal_default(&cfg->touch);
  cfg->clock_offset = 0;
}

static s16 get16(const u8 *p) { return (s16)(p[0] | (p[1] << 8)); }

static void put16(u8 *p, s16 v) {
  p[0] = (u8)((u16)v & 0xFF);
  p[1] = (u8)((u16)v >> 8);
}

int user_config_load(UserConfig *cfg) {
  user_config_defaults(cfg);

  if (f_mount(&s_fs, "", 1) != FR_OK)
    return 0;
  FRESULT fr = f_open(&s_fil, USER_DAT_PATH, FA_READ);
  if (fr != FR_OK) {
    f_mount(NULL, "", 0);
    return 0;
  }
  UINT br = 0;
  fr = f_read(&s_fil, s_buf, USER_DAT_SIZE, &br);
  f_close(&s_fil);
  f_mount(NULL, "", 0);
  if (fr != FR_OK || br < 12)
    return 0;

  if (s_buf[0] != 'A' || s_buf[1] != 'D' || s_buf[2] != 'A' || s_buf[3] != 'T')
    return 0;

  cfg->setup_done = s_buf[5];
  cfg->language = s_buf[6];
  cfg->accent = s_buf[7];
  cfg->birth_day = s_buf[8];
  cfg->birth_month = s_buf[9];
  cfg->birth_year = (u16)(s_buf[10] | (s_buf[11] << 8));
  int i = 0;
  for (; i < USER_NAME_MAX - 1 && (12 + i) < (int)br; i++)
    cfg->name[i] = (char)s_buf[12 + i];
  cfg->name[i] = '\0';

  if (br >= USER_DAT_TOUCH + 9u && s_buf[USER_DAT_TOUCH]) {
    const u8 *t = s_buf + USER_DAT_TOUCH + 1;
    cfg->touch.x_min = get16(t);
    cfg->touch.x_max = get16(t + 2);
    cfg->touch.y_min = get16(t + 4);
    cfg->touch.y_max = get16(t + 6);
    cfg->touch_set = 1;
  }

  if (br >= USER_DAT_CLOCK + 2u) {
    cfg->clock_offset = get16(s_buf + USER_DAT_CLOCK);
    if (cfg->clock_offset <= -1440 || cfg->clock_offset >= 1440)
      cfg->clock_offset = 0;
  }

  if (cfg->language >= LANG_COUNT)
    cfg->language = LANG_ENGLISH;
  if (cfg->accent >= AURORA_ACCENT_COUNT)
    cfg->accent = 0;
  clamp_date(cfg);
  cfg->valid = 1;
  return 1;
}

int user_config_save(const UserConfig *cfg) {
  if (f_mount(&s_fs, "", 1) != FR_OK)
    return 0;
  f_mkdir(USER_DAT_DIR); /* ignore FR_EXIST / already-present */

  for (int i = 0; i < USER_DAT_SIZE; i++)
    s_buf[i] = 0;
  s_buf[0] = 'A';
  s_buf[1] = 'D';
  s_buf[2] = 'A';
  s_buf[3] = 'T';
  s_buf[4] = USER_DAT_VERSION;
  s_buf[5] = cfg->setup_done;
  s_buf[6] = cfg->language;
  s_buf[7] = cfg->accent;
  s_buf[8] = cfg->birth_day;
  s_buf[9] = cfg->birth_month;
  s_buf[10] = (u8)(cfg->birth_year & 0xFF);
  s_buf[11] = (u8)(cfg->birth_year >> 8);
  for (int i = 0; i < USER_NAME_MAX; i++)
    s_buf[12 + i] = (u8)cfg->name[i];
  put16(s_buf + USER_DAT_CLOCK, cfg->clock_offset);
  if (cfg->touch_set) {
    u8 *t = s_buf + USER_DAT_TOUCH;
    t[0] = 1;
    put16(t + 1, cfg->touch.x_min);
    put16(t + 3, cfg->touch.x_max);
    put16(t + 5, cfg->touch.y_min);
    put16(t + 7, cfg->touch.y_max);
  }

  FRESULT fr = f_open(&s_fil, USER_DAT_PATH, FA_WRITE | FA_CREATE_ALWAYS);
  if (fr != FR_OK) {
    f_mount(NULL, "", 0);
    return 0;
  }
  UINT bw = 0;
  fr = f_write(&s_fil, s_buf, USER_DAT_SIZE, &bw);
  f_close(&s_fil);
  f_mount(NULL, "", 0);
  return (fr == FR_OK && bw == USER_DAT_SIZE);
}
