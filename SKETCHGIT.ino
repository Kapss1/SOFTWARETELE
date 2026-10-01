#include <WiFi.h>
#include <ThingSpeak.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <BH1750.h>
#include <DHT.h>

//================ PINES ==================
#define DHTPIN 25
#define DHTTYPE DHT22
#define DSM_PIN 35        // Solo entrada (GPIO34-39 no soportan salida ni pull-up interno)
#define BUZZER_PIN 18     // Controla la base del 2N2222 a través de una R de 1k
#define I2C_SDA 27
#define I2C_SCL 26

//================ WIFI ==================
const char* ssid = "";
const char* password = "";

//============== THINGSPEAK ==============
unsigned long channelID = ; // channel ID
const char* writeAPIKey = "";

//============== TELEGRAM ================
String botToken = "";
String chatID = "";
bool telegramEnviado = false;

//=============== SENSORES ===============
DHT dht(DHTPIN, DHTTYPE);
BH1750 luz;
WiFiClient client;

//============= DSM501A (por interrupcion) ==================
volatile unsigned long lowPulseOccupancy = 0; // suma de tiempo en LOW (microsegundos)
volatile unsigned long lastChangeTime = 0;
volatile bool pinState = HIGH;

unsigned long inicio = 0;
const unsigned long sampleTime = 30000; // 30 segundos
float polvo = 0;

void IRAM_ATTR dsmISR() {
  unsigned long now = micros();
  bool nivelActual = digitalRead(DSM_PIN);

  if (pinState == HIGH && nivelActual == LOW) {
    // acaba de empezar un pulso LOW
    lastChangeTime = now;
  }
  else if (pinState == LOW && nivelActual == HIGH) {
    // el pulso LOW termino, sumamos su duracion
    lowPulseOccupancy += (now - lastChangeTime);
  }
  pinState = nivelActual;
}

//============= ALARMA ===================
// Umbrales calibrados con mediciones reales (aire normal: 270-670, humo: ~25000)
const float umbralPrecaucion = 1000.0;
const float umbralPolvo = 3000.0;
bool alarma = false;
byte contadorAlarma = 0;
const byte lecturasNecesarias = 2;

//============= TELEGRAM =================
void enviarTelegram(String mensaje){
  if(WiFi.status()==WL_CONNECTED){
    HTTPClient http;
    mensaje.replace(" ","%20");
    mensaje.replace("\n","%0A");
    String url = "https://api.telegram.org/bot"
                 + botToken +
                 "/sendMessage?chat_id="
                 + chatID +
                 "&text="
                 + mensaje;
    http.begin(url);
    int respuesta = http.GET();
    if(respuesta == 200){
      Serial.println("Telegram enviado correctamente");
    }
    else{
      Serial.print("Error Telegram: ");
      Serial.println(respuesta);
    }
    http.end();
  }
}

//================ SETUP ==================
void setup(){
  Serial.begin(115200);
  dht.begin();

  Wire.begin(I2C_SDA, I2C_SCL);
  if(luz.begin()){
    Serial.println("BH1750 listo");
  }
  else{
    Serial.println("Error BH1750");
  }

  // DSM501A: solo entrada, capturado por interrupcion
  pinMode(DSM_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(DSM_PIN), dsmISR, CHANGE);

  // Buzzer via transistor 2N2222 (HIGH = encendido)
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  WiFi.begin(ssid,password);
  Serial.print("Conectando WiFi");
  while(WiFi.status()!=WL_CONNECTED){
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi conectado");

  ThingSpeak.begin(client);
  inicio = millis();
}

//================ LOOP ==================
void loop(){
  float temp = dht.readTemperature();
  float hum = dht.readHumidity();
  float lux = luz.readLightLevel();

  if(millis()-inicio >= sampleTime){

    noInterrupts();
    unsigned long lecturaLow = lowPulseOccupancy;
    lowPulseOccupancy = 0;
    interrupts();

    float ratio = lecturaLow / (sampleTime * 10.0); // % de tiempo en LOW

    polvo = 1.1*pow(ratio,3)
          - 3.8*pow(ratio,2)
          + 520*ratio
          + 0.62;

    inicio = millis();

    //=========== VALIDACION DHT22 ===========
    bool dhtValido = !(isnan(temp) || isnan(hum));
    if(!dhtValido){
      Serial.println("Error leyendo DHT22, se omite el envio de temp/hum");
    }

    //=========== CALIDAD DEL AIRE ===========
    if(polvo < umbralPrecaucion){
      alarma = false;
      contadorAlarma = 0;
      telegramEnviado = false;
    }
    else if(polvo < umbralPolvo){
      alarma = false;
      contadorAlarma = 0;
    }
    else{
      contadorAlarma++;
      Serial.print("Lecturas altas consecutivas: ");
      Serial.println(contadorAlarma);

      if(contadorAlarma >= lecturasNecesarias){
        alarma = true;

        if(!telegramEnviado){
          String mensaje = "ALERTA AMBIENTAL\n";
          mensaje += "Material particulado elevado: ";
          mensaje += String(polvo);
          mensaje += " pcs/0.01ft3\n";
          mensaje += "Temperatura: ";
          mensaje += dhtValido ? String(temp) : "N/D";
          mensaje += " C\n";
          mensaje += "Humedad: ";
          mensaje += dhtValido ? String(hum) : "N/D";
          mensaje += " %\n";
          mensaje += "Luz: ";
          mensaje += String(lux);
          mensaje += " lux";
          enviarTelegram(mensaje);
          telegramEnviado = true;
        }
      }
    }

    digitalWrite(BUZZER_PIN, alarma ? HIGH : LOW);

    //=========== SERIAL ===========
    Serial.println("--------------------------");
    Serial.print("Temperatura: ");
    Serial.print(temp);
    Serial.println(" C");
    Serial.print("Humedad: ");
    Serial.print(hum);
    Serial.println(" %");
    Serial.print("Luz: ");
    Serial.print(lux);
    Serial.println(" lux");
    Serial.print("Material particulado: ");
    Serial.print(polvo);
    Serial.println(" pcs/0.01ft3");

    if(alarma){
      Serial.println("***** ALERTA ACTIVADA *****");
    }
    else if(polvo >= umbralPrecaucion){
      Serial.println("Precaucion: nivel elevado");
    }
    else{
      Serial.println("Estado normal");
    }

    //=========== WIFI =================
    if(WiFi.status()!=WL_CONNECTED){
      Serial.println("Reconectando WiFi...");
      WiFi.disconnect();
      WiFi.begin(ssid,password);
      while(WiFi.status()!=WL_CONNECTED){
        delay(500);
        Serial.print(".");
      }
      Serial.println("\nWiFi reconectado");
    }

    //=========== THINGSPEAK ===========
    Serial.println("Enviando a ThingSpeak");
    if(dhtValido){
      ThingSpeak.setField(1,temp);
      ThingSpeak.setField(2,hum);
    }
    ThingSpeak.setField(3,lux);
    ThingSpeak.setField(4,polvo);
    ThingSpeak.setField(5,alarma);
    int estado = ThingSpeak.writeFields(channelID,writeAPIKey);
    if(estado==200){
      Serial.println("Datos enviados correctamente");
    }
    else{
      Serial.print("Error envio: ");
      Serial.println(estado);
    }
  }

  delay(100);
}