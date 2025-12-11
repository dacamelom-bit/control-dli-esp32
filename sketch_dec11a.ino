/*
 * Versión Actualizada:
 * - Control DLI con REINICIO AUTOMÁTICO (Ciclo de Cultivo + Descanso).
 * - DLI Acumulado llega a la meta -> Apaga luces -> Espera 8 horas -> Reinicia a 0.
 * - Persistencia en NVS.
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// --- 1. CONFIGURACIÓN DE WIFI ---
const char* ssid     = "Paz_Salas_LAB_INF";
const char* password = "S4l4sLab#";

// --- 2. CONFIGURACIÓN DE SUPABASE ---
String supabase_url = "https://xjokrbhvbdbsvxatkzaz.supabase.co";
String supabase_key = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Inhqb2tyYmh2YmRic3Z4YXRremF6Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjE1MDUxMjQsImV4cCI6MjA3NzA4MTEyNH0.MG9izfNT9XRxNQN1O2h98z4g41S1caAYjQBbm0xLwgY";

// --- 3. CONFIGURACIÓN DE RELÉS ---
const int RELAY_BED1_PIN   = 16;
const int RELAY_BED2_PIN   = 17;
const int RELAY_ACTIVE     = LOW;
const int RELAY_INACTIVE   = HIGH;

// --- 4. CONFIGURACIÓN DE TIEMPO DE CICLO ---
const long interval       = 60000; // 1 minuto
unsigned long previousMillis = 0;

// =================== CONTROL POR DLI Y DESCANSO ===================
// Meta de "dosis" de luz para el orégano
const float OREGANO_DLI_TARGET = 12.0; // mol/m²

// TIEMPO DE DESCANSO (Oscuridad obligatoria tras completar DLI)
// 8 horas = 8 * 60 * 60 * 1000 milisegundos
const unsigned long REST_DURATION_MS = 8UL * 60UL * 60UL * 1000UL; 

// Anti-parpadeo del relé
const unsigned long MIN_ON_MS  = 5UL * 60UL * 1000UL; 
const unsigned long MIN_OFF_MS = 2UL * 60UL * 1000UL; 

// --- Persistencia ---
Preferences prefs;
const bool RESET_STATE_AT_START = false; // Poner en true una vez para borrar todo

// --- Variables globales ---
HTTPClient http;

float accumulatedDLI      = 0.0; // DLI del ciclo actual
float ledOnMinutesToday   = 0.0; // Estadística diaria
String currentDate        = "";

// Variables de Estado
bool lampsOn              = false;
unsigned long lastSwitchMs = 0;

// Variables para el Modo Descanso
bool isResting            = false;
unsigned long restStartTime = 0;

// ----------------------------------------------------------------------
// WIFI
// ----------------------------------------------------------------------
void setup_wifi() {
    delay(10);
    Serial.print("Conectando a ");
    Serial.println(ssid);
    WiFi.begin(ssid, password);

    int retries = 0;
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
        retries++;
        if (retries > 40) {
            Serial.println("\nNo se pudo conectar. Reiniciando...");
            ESP.restart();
        }
    }
    Serial.println("\nWiFi conectado!");
    Serial.println(WiFi.localIP());
}

// ----------------------------------------------------------------------
// Manejo de relés con anti-parpadeo
// ----------------------------------------------------------------------
void setLamps(bool on) {
    unsigned long nowMs = millis();
    if (on == lampsOn) return;

    if (on) {
        if (nowMs - lastSwitchMs < MIN_OFF_MS) return;
    } else {
        if (nowMs - lastSwitchMs < MIN_ON_MS) return;
    }

    digitalWrite(RELAY_BED1_PIN, on ? RELAY_ACTIVE : RELAY_INACTIVE);
    digitalWrite(RELAY_BED2_PIN, on ? RELAY_ACTIVE : RELAY_INACTIVE);

    lampsOn = on;
    lastSwitchMs = nowMs;
}

// ----------------------------------------------------------------------
// Persistencia en NVS
// ----------------------------------------------------------------------
void loadStateFromPrefs() {
    accumulatedDLI    = prefs.getFloat("dli_cycle", 0.0f);
    ledOnMinutesToday = prefs.getFloat("ledmins", 0.0f);
    currentDate       = prefs.getString("date", "");
    
    // Cargar estado de descanso
    isResting         = prefs.getBool("resting", false);

    Serial.println("Estado restaurado desde NVS:");
    Serial.print("  DLI Acumulado: "); Serial.println(accumulatedDLI);
    Serial.print("  En descanso: "); Serial.println(isResting ? "SI" : "NO");
}

void saveStateToPrefs() {
    prefs.putFloat("dli_cycle", accumulatedDLI);
    prefs.putFloat("ledmins", ledOnMinutesToday);
    prefs.putString("date", currentDate);
    prefs.putBool("resting", isResting);
}

// ----------------------------------------------------------------------
// LECTURA SUPABASE
// ----------------------------------------------------------------------
float fetchReferencePPFD() {
    String api_url = supabase_url 
        + "/rest/v1/sensor_readings?sensor_id=eq.Referencia"
          "&select=ppfd_total,created_at"
          "&order=created_at.desc&limit=1";

    Serial.println("  > Consultando Supabase...");
    http.begin(api_url);
    http.addHeader("apikey", supabase_key);
    http.addHeader("Authorization", "Bearer " + supabase_key);
    
    int httpResponseCode = http.GET();
    float ppfd = -1.0;

    if (httpResponseCode == 200) {
        String payload = http.getString();
        DynamicJsonDocument doc(1024);
        deserializeJson(doc, payload);
        JsonArray arr = doc.as<JsonArray>();

        if (arr.size() > 0) {
            ppfd = arr[0]["ppfd_total"].as<float>();
            String created = arr[0]["created_at"].as<String>();
            
            // Gestión de cambio de día para estadísticas
            if (created.length() >= 10) {
                String dateStr = created.substring(0, 10);
                if (currentDate == "") {
                    currentDate = dateStr;
                } else if (dateStr != currentDate) {
                    currentDate = dateStr;
                    ledOnMinutesToday = 0.0f; // Reinicia solo la estadística diaria
                    saveStateToPrefs();
                }
            }
        }
    } else {
        Serial.print("Error HTTP: ");
        Serial.println(httpResponseCode);
    }
    http.end();
    return ppfd;
}

// ----------------------------------------------------------------------
// LÓGICA MAESTRA: DLI + DESCANSO (RESET)
// ----------------------------------------------------------------------
void masterControlDLI(float ppfd_reading) {
    unsigned long currentMs = millis();

    // --- CASO 1: ESTAMOS EN MODO DESCANSO ---
    if (isResting) {
        setLamps(false); // Asegurar luces apagadas
        
        unsigned long elapsedRest = currentMs - restStartTime;
        unsigned long restMinutes = elapsedRest / 60000;
        unsigned long targetMinutes = REST_DURATION_MS / 60000;

        Serial.print("  [MODO DESCANSO] Tiempo: ");
        Serial.print(restMinutes);
        Serial.print(" / ");
        Serial.print(targetMinutes);
        Serial.println(" min.");

        // Verificar si el descanso terminó
        if (elapsedRest >= REST_DURATION_MS) {
            Serial.println("  >>> FIN DEL DESCANSO. INICIANDO NUEVO CICLO <<<");
            
            accumulatedDLI = 0.0;     // Reiniciar DLI
            isResting = false;        // Salir de descanso
            // Nota: ledOnMinutesToday no se reinicia aquí, eso depende del día calendario
            
            saveStateToPrefs();
        }
        return; // Salir, no acumulamos luz mientras descansamos
    }

    // --- CASO 2: MODO ACTIVO (Acumulando DLI) ---
    if (ppfd_reading < 0) return; // Error de lectura

    // Integrar DLI
    const float dt_seconds = interval / 1000.0f;
    accumulatedDLI += (ppfd_reading * dt_seconds) / 1000000.0f;

    // Verificar meta
    if (accumulatedDLI >= OREGANO_DLI_TARGET) {
        Serial.println("  [CONTROL DLI] Meta alcanzada (12.0). Entrando en REPOSO.");
        setLamps(false);
        
        isResting = true;
        restStartTime = currentMs; // Guardar hora de inicio del descanso
    } else {
        Serial.println("  [CONTROL DLI] Falta luz. Lámparas ENCENDIDAS.");
        setLamps(true);
        
        if (lampsOn) {
            ledOnMinutesToday += (interval / 60000.0f);
        }
    }

    // Logs
    Serial.print("  > DLI Ciclo: ");
    Serial.print(accumulatedDLI, 4);
    Serial.print(" / ");
    Serial.println(OREGANO_DLI_TARGET, 2);

    saveStateToPrefs();
}

// ----------------------------------------------------------------------
// SETUP
// ----------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("Iniciando Controlador - DLI con Auto-Reset...");

    prefs.begin("solartrace", false);
    if (RESET_STATE_AT_START) {
        prefs.clear();
        Serial.println("NVS Borrada.");
    }
    loadStateFromPrefs();

    // Si arrancamos y estaba en descanso, reiniciamos el contador de descanso 
    // (por seguridad, para garantizar las 8 horas si hubo corte de luz)
    if (isResting) {
        restStartTime = millis(); 
        Serial.println("Reanudando periodo de descanso...");
    }

    setup_wifi();

    pinMode(RELAY_BED1_PIN, OUTPUT);
    pinMode(RELAY_BED2_PIN, OUTPUT);
    digitalWrite(RELAY_BED1_PIN, RELAY_INACTIVE);
    digitalWrite(RELAY_BED2_PIN, RELAY_INACTIVE);

    lampsOn      = false;
    lastSwitchMs = millis();
}

// ----------------------------------------------------------------------
// LOOP
// ----------------------------------------------------------------------
void loop() {
    unsigned long currentMillis = millis();

    if (currentMillis - previousMillis >= interval) {
        previousMillis = currentMillis;

        Serial.println("\n--- CICLO DE CONTROL ---");

        if (WiFi.status() != WL_CONNECTED) setup_wifi();

        // Solo leemos sensores si estamos activos (ahorrar datos en descanso opcional)
        // Pero mejor leer siempre para monitoreo
        float latest_ppfd = fetchReferencePPFD();

        masterControlDLI(latest_ppfd);

        Serial.println("--- FIN CICLO ---");
    }
}