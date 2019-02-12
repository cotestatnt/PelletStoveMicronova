#include <FS.h>
#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>

#include <ArduinoJson.h>        //https://github.com/bblanchon/ArduinoJson
#include <WiFiManager.h>        //https://github.com/tzapu/WiFiManager
#include <CTBot.h>              //https://github.com/shurillu/CTBot

#include <SoftwareSerial.h>
SoftwareSerial StoveSerial(D1, D0, false, 32);

#define txPin D0
#define rxPin D1
#define extInput D7
#define ResetWiFi D2
#define OFF LOW
#define ON HIGH

bool extRising = false, extFalling = false;

const char StoveOn[4] = { 0x80, 0x21, 0x01, 0xA2 };
const char StoveOff[4] = { 0x80, 0x21, 0x06, 0xA7 };
/*
I comandi di lettura sono così strutturati:
- un byte per indicare il banco di memoria da leggere (00h per la RAM, 20h per la EPROM)
- un byte per l'indirizzo da leggere (da 00h a FFh)
la risposta è così composta:
- un byte di CRC, come sommatoria fra i due byte di cui sopra ed il byte di valore restituito (vedi sotto), in modulo FFh
- un byte contenente il valore richiesto
 */

#define ParStoveStatus 0x21
 /* (0 - off, 1 - starting, 2 - load pellet, 3 - flame light, 4 - work, 5 - cleaning, 6 - off, 7 - cleaning final) */
#define ParAmbTemp 0x01
#define ParWaterTemp 0x03
#define ParFumeTemp 0x5A
#define ParStovePower 0x34

uint8_t TempAmbient, TempFumes, StovePower, TempWater;
uint8_t stoveState = 0, lastStoveVal = 0;
bool ack = false;
uint32_t replyDelay = 200;
char stoveRxData[2];

uint8_t ON_TEMP = 70;
#define TELEGRAM_TIME 500
#define REPLY_TIME 1000

#define START_CALLBACK  "Comando bruciatore ON" 
#define STOP_CALLBACK "Comando bruciatore OFF"
#define STATE_CALLBACK "Stato bruciatore"
#define CONFIRM_CALLBACK "Esegui"
#define CANCEL_CALLBACK "Annulla"

enum { WAIT = -1, STOP = 0, START = 1, STATE = 3 };
typedef struct {
  int request = WAIT;
  bool confirm = false;
  uint16_t value1;
  uint16_t value2;
} telegramCmd;

const uint8_t OUT = D6;
uint32_t checkTelegramTime, stoveReplyTime, checkTempTime, startCheckTime, updateTime;

// Telegram vars
CTBot myBot;
CTBotInlineKeyboard myKbd, goKbd;  // custom inline keyboard object helper
String token;
TBMessage msg;
telegramCmd cmdMsg;
bool shouldSaveConfig = false, connected = false;
bool captive = false;

void setup() {
  pinMode(OUT, OUTPUT);
  digitalWrite(OUT, HIGH);
  pinMode(extInput, INPUT_PULLUP);
  pinMode(ResetWiFi, INPUT_PULLUP);
  
  Serial.begin(1200);
  StoveSerial.begin(1200);
  StoveSerial.enableIntTx(false);
  Serial.println("\n\n");

  Serial.print("Mounting FS...");
  if (SPIFFS.begin()) {
    Serial.println(" done");
    if (SPIFFS.exists("/config.json")) {
      //file exists, reading and loading
      Serial.print("Reading config file..");
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile) {
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);
        configFile.readBytes(buf.get(), size);
        DynamicJsonBuffer jsonBuffer;
        JsonObject& json = jsonBuffer.parseObject(buf.get());
        //json.printTo(Serial);
        if (json.success()) {
          token = json["token"].as<String>();          
          ON_TEMP = json["ON_TEMP"];
          checkTempTime = json["checkTempTime"] ;
          Serial.println(" done");
        }
        else {
          Serial.println(" failed to parse json config");
        }
        configFile.close();
      }
    }    
  }
  else {
    Serial.println(" failed to mount FS");
  }
  
  Serial.printf("\n\nTelegram token: %s", token.c_str());
  Serial.printf("\nON Temperature setpoint: %u °C", ON_TEMP);
  Serial.printf("\nON timeout check: %u ms.", checkTempTime);

  WiFi.mode(WIFI_STA);
  WiFi.reconnect();
  delay(2000);  

  if (WiFi.isConnected()) {              
    onConnection(WIFI_EVENT_STAMODE_GOT_IP);
  } 
  getStoveState();
}

void loop() {
  if (WiFi.isConnected() &! connected) 
    onConnection(WIFI_EVENT_STAMODE_GOT_IP);
  else if (!WiFi.isConnected() && connected)
    onDisconnection( WIFI_EVENT_STAMODE_DISCONNECTED);

  checkStoveReply();

  if (digitalRead(extInput) == LOW && !extRising) {
    extRising = true;
    cmdMsg.confirm = true;
    cmdMsg.request = START;
    startCheckTime = millis();    
    Serial.println("START from external signal");
    delay(100);
  }

  if (digitalRead(extInput) && extRising) {
    extRising = false;
    cmdMsg.request = STOP;    
    cmdMsg.confirm = true;
    Serial.println("STOP from external signal");
    delay(100);
  }

  while (Serial.available()) {
    StoveSerial.write(Serial.read());
  }

  if (millis() - checkTelegramTime > TELEGRAM_TIME) {
    checkTelegramTime = millis();
    checkTelegramKbd();
  }

  if ((millis() - stoveReplyTime) > REPLY_TIME && lastStoveVal != 0) {
    if (lastStoveVal == stoveRxData[1]) {
      ack = true;
      Serial.println("Comando ricevuto dalla stufa");
    }
    else {
      ack = false;
      Serial.println("Errore trasmissione comando alla stufa");
      Serial.println(lastStoveVal, HEX);
      Serial.println(stoveRxData[1], HEX);
    }
    lastStoveVal = 0;
  }

  if (cmdMsg.request == START && (millis() - startCheckTime > checkTempTime)) {
    cmdMsg.request = WAIT;
    char tx_buffer[80];
    if (TempWater > ON_TEMP) {
      sprintf(tx_buffer, "Tutto ok!\nTemperatura acqua: %02d°C\n", TempWater);
      myBot.sendMessage(msg.sender.id, tx_buffer);
      Serial.println(tx_buffer);      
    }
    else {
      sprintf(tx_buffer, "Attenzione!\nC'è qualcosa che non torna... \nTemperatura acqua: %02d°C\n", TempWater);
      myBot.sendMessage(msg.sender.id, tx_buffer);
      Serial.println(tx_buffer);      
    }
  }

  if (cmdMsg.request == START && (millis() - updateTime) > 10000) {
    updateTime = millis();
    getWaterTemp();
  }


  if (cmdMsg.confirm == true) {
    cmdMsg.confirm = false;
    if (cmdMsg.request == START) {
      for (int i = 0; i < 4; i++) {
        StoveSerial.write(StoveOn[i]);
        delayMicroseconds(800);
      }
      lastStoveVal = StoveOn[2];
      stoveReplyTime = millis();
      updateTime = millis();
    }
    else if (cmdMsg.request == STOP) {
      for (int i = 0; i < 4; i++) {
        StoveSerial.write(StoveOff[i]);
        delayMicroseconds(800);
      }
      cmdMsg.request = WAIT;
      lastStoveVal = StoveOff[2];
      stoveReplyTime = millis();
    }
  }

  if (shouldSaveConfig) {
    //save the custom parameters to FS  
    shouldSaveConfig = false;
    Serial.println("Saving config..");
    DynamicJsonBuffer jsonBuffer;
    JsonObject& json = jsonBuffer.createObject();
    json["token"] = token;
    json["ON_TEMP"] = ON_TEMP;
    json["checkTempTime"] = checkTempTime;
    File configFile = SPIFFS.open("/config.json", "w");
    if (!configFile) { Serial.println("failed to open config file for writing"); }
    json.printTo(Serial);
    json.printTo(configFile);
    configFile.close();
    Serial.println(" done.");
  }

  
  if (digitalRead(ResetWiFi) == LOW &! captive) {
    captivePortal();    
  }

}


void checkStoveReply() {
  uint8_t rxCount = 0;
  stoveRxData[0] = 0x00;  stoveRxData[1] = 0x00;
  while (StoveSerial.available()) {
    stoveRxData[rxCount] = StoveSerial.read();
    //Serial.write(stoveRxData[rxCount]);
    rxCount++;
  }
  if (rxCount == 2) {
    byte param = stoveRxData[0] - stoveRxData[1];
    byte val = stoveRxData[1];
    switch (param) {
    case ParStoveStatus:
      stoveState = val;
      Serial.printf("Stufa %s\n", stoveState ? "ON" : "OFF");
      break;
    case ParAmbTemp:
      TempAmbient = val / 2;
      Serial.printf("T. amb. %d\n", TempAmbient);
      break;
    case ParWaterTemp:
      TempWater = val;
      Serial.printf("T. acqua. %d\n", TempWater);
      break;
    case ParStovePower:
      StovePower = val;
      Serial.printf("Potenza %d\n", StovePower);
      break;
    case ParFumeTemp:
      TempFumes = val;
      Serial.printf("T. fumi %d\n", TempFumes);
      break;
    }
  }
}


void getStoveState() {
  const char readByte = 0x00;
  StoveSerial.write(readByte);
  delay(1);
  StoveSerial.write(ParStoveStatus);
  delay(replyDelay);
  checkStoveReply();

  StoveSerial.write(readByte);
  delay(1);
  StoveSerial.write(ParAmbTemp);
  delay(replyDelay);
  checkStoveReply();
}

void getFumeTemp() {
  const char readByte = 0x00;
  StoveSerial.write(readByte);
  delay(1);
  StoveSerial.write(ParFumeTemp);
  delay(replyDelay);
  checkStoveReply();
}

void getWaterTemp() {
  const char readByte = 0x00;
  StoveSerial.write(readByte);
  delay(1);
  StoveSerial.write(ParWaterTemp);
  delay(replyDelay);
  checkStoveReply();
}

void getStovePower() {
  const char readByte = 0x00;
  StoveSerial.write(readByte);
  delay(1);
  StoveSerial.write(ParStovePower);
  delay(replyDelay);
  checkStoveReply();
}


void saveConfigCallback() {
  Serial.println("Configurazione salvata.");
  shouldSaveConfig = true;
}


void onConnection(WiFiEvent_t event) {
  Serial.print("\nWiFi connesso, Indirizzo IP: ");
  Serial.println(WiFi.localIP());
  connected = true;
  
  //myBot.wifiConnect(ssid, pass);
  myBot.setTelegramToken(token);
  myBot.testConnection();
  Serial.print("\nTelegram test connessione 1 ");
  Serial.println(myBot.testConnection() ? "OK" : "NOT OK");  
  Serial.print("Telegram test connessione 2 ");
  Serial.println(myBot.testConnection() ? "OK" : "NOT OK");
  myKbd.flushData();
  myKbd.addButton("START", START_CALLBACK, CTBotKeyboardButtonQuery);
  myKbd.addButton("STOP", STOP_CALLBACK, CTBotKeyboardButtonQuery);
  myKbd.addRow();
  myKbd.addButton("Stato attuale", STATE_CALLBACK, CTBotKeyboardButtonQuery);
  goKbd.flushData();
  goKbd.addButton("ESEGUI", CONFIRM_CALLBACK, CTBotKeyboardButtonQuery);
  goKbd.addButton("ANNULLA", CANCEL_CALLBACK, CTBotKeyboardButtonQuery);
}

void onDisconnection(WiFiEvent_t event) {
  connected = false;
  Serial.println("WiFi disconnected.");  
  Serial.println(event);
}


void checkTelegramKbd() {
  //TBMessage msg;
#define TX_SIZE 120
  if (myBot.getNewMessage(msg)) {
    char tx_buffer[TX_SIZE];
    if (msg.messageType == CTBotMessageText) {
      if (msg.text.equalsIgnoreCase("/token")) {        
        sprintf(tx_buffer, "Il token di questa chat è: \n%s\n", token.c_str());
        myBot.sendMessage(msg.sender.id, tx_buffer);
      }
      if (msg.text.equalsIgnoreCase("/acqua")) {
        getWaterTemp();
        sprintf(tx_buffer, "La temperatura dell'acqua è %02d°C\n", TempWater);
        myBot.sendMessage(msg.sender.id, tx_buffer);
        Serial.println("Telegram /acqua command received");
      }
      else if (msg.text.equalsIgnoreCase("/fumi")) {
        getFumeTemp();
        sprintf(tx_buffer, "La temperatura dei fumi è %02d°C\n", TempFumes);
        myBot.sendMessage(msg.sender.id, tx_buffer);
        Serial.println("Telegram /fumi command received");
      }
      else if (msg.text.equalsIgnoreCase("/potenza")) {
        getStovePower();
        sprintf(tx_buffer, "La potenza della stufa è %02d\n", StovePower);
        myBot.sendMessage(msg.sender.id, tx_buffer);
        Serial.println("Telegram /potenza command received");
      }
      else if (msg.text.equalsIgnoreCase("/help")) {
        sprintf(tx_buffer, "%s%s%s", "/acqua per leggere la temperatura dell'acqua\n",
          "/fumi per leggere la temperatura dei fumi\n",
          "/potenza per conoscere la potenza di riscaldmanento attuale.");
        myBot.sendMessage(msg.sender.id, tx_buffer);
        Serial.println("Telegram /help command received");
      }
      else {
        sprintf(tx_buffer, "Ciao %s!\nLa tua stufa è %s.\nTemperatura acqua: %02d°C\n/help per l'elenco dei comandi disponibili.",
          msg.sender.username.c_str(), stoveState ? "accesa" : "spenta", TempWater);
        myBot.sendMessage(msg.sender.id, tx_buffer, myKbd);
      }
    }
    else if (msg.messageType == CTBotMessageQuery) {
      if (msg.callbackQueryData.equals(START_CALLBACK)) {
        cmdMsg.confirm = false;
        cmdMsg.request = START;
        startCheckTime = millis();
        myBot.endQuery(msg.callbackQueryID, "Comando di accensione ricevuto..");
        myBot.sendMessage(msg.sender.id, "Confermi l'esecuzione del comando?", goKbd);
        Serial.println("Telegram START command received");
      }
      else if (msg.callbackQueryData.equals(STOP_CALLBACK)) {
        cmdMsg.confirm = false;
        cmdMsg.request = STOP;
        myBot.endQuery(msg.callbackQueryID, "Comando di spegnimento ricevuto..");
        myBot.sendMessage(msg.sender.id, "Confermi l'esecuzione del comando?", goKbd);
        Serial.println("Telegram STOP command received");
      }
      else if (msg.callbackQueryData.equals(STATE_CALLBACK)) {
        cmdMsg.confirm = false;
        getStoveState();
        sprintf(tx_buffer, "Stufa %s.\nTemperatura ambiente: %02d°C\n",
          stoveState ? "accesa" : "spenta", TempAmbient);
        myBot.sendMessage(msg.sender.id, tx_buffer);
        myBot.endQuery(msg.callbackQueryID, "");
        Serial.println("Telegram STATE command received");
      }
      else if (msg.callbackQueryData.equals(CONFIRM_CALLBACK)) {
        cmdMsg.confirm = true;
        startCheckTime = millis();
        myBot.endQuery(msg.callbackQueryID, "");
        if (cmdMsg.request != WAIT)
          myBot.sendMessage(msg.sender.id, "Comando eseguito.");
        else
          myBot.sendMessage(msg.sender.id, "Non c'è nessun comando da eseguire.");
        Serial.println("Telegram \"confirm\" command received");
      }
      else if (msg.callbackQueryData.equals(CANCEL_CALLBACK)) {
        cmdMsg.confirm = false;
        myBot.endQuery(msg.callbackQueryID, "");
        if (cmdMsg.request != WAIT)
          myBot.sendMessage(msg.sender.id, "Comando annullato.");
        else
          myBot.sendMessage(msg.sender.id, "Non c'è nessun comando da annullare.");
        Serial.println("Telegram \"cancel\" command received");
      }
    }
  }

}



void captivePortal() {
  captive = true;
  //WiFi.disconnect();
  WiFi.mode(WIFI_AP);
  Serial.println("Start captive portal");
  WiFiManager wifiManager;
  wifiManager.setBreakAfterConfig(true);
  //wifiManager.setDebugOutput(false);
  wifiManager.setSaveConfigCallback(saveConfigCallback);
  wifiManager.setConfigPortalTimeout(300);

  WiFiManagerParameter telegramLabel("<p>Telegram token</p>");
  WiFiManagerParameter telegramToken("Telegram", "Token", token.c_str(), 50);
  WiFiManagerParameter checkTempLabel("<p>Tempo di controllo accensione (s)</p>");
  WiFiManagerParameter checkTempSec("tempoFumi", "Controllo Acqua", "1200", 50);
  WiFiManagerParameter valueTempLabel("<p>Setpoint controllo temperatura acqua (°C)</p>");
  WiFiManagerParameter valueTemp("setpoint", "Temperatura acqua", "80", 50);

  wifiManager.addParameter(&telegramLabel);
  wifiManager.addParameter(&telegramToken);
  wifiManager.addParameter(&checkTempLabel);
  wifiManager.addParameter(&checkTempSec);
  wifiManager.addParameter(&valueTempLabel);
  wifiManager.addParameter(&valueTemp);
  //wifiManager.autoConnect();

  if (!wifiManager.startConfigPortal()) {
    Serial.println("failed to connect and hit timeout");
    delay(3000);
    //reset and try again, or maybe put it to deep sleep
    ESP.reset();
    delay(5000);
  }

  //read updated parameters
  token = telegramToken.getValue();
  ON_TEMP = String(valueTemp.getValue()).toInt();
  checkTempTime = String(checkTempSec.getValue()).toInt() * 1000;
  Serial.println("Updated paramenter: ");
  Serial.println(token);
  Serial.println(ON_TEMP);
  Serial.println(checkTempTime);
  shouldSaveConfig = true;
  captive = false;
  WiFi.mode(WIFI_STA);
}
