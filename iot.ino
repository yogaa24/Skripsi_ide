// Include untuk menentukan jalur pin sensor PZEM ke ESP8266
#include <PZEM004Tv30.h>
PZEM004Tv30 pzem(D5, D6); // Menggunakan hanya satu sensor PZEM

//Include untuk mengaktifkan atau koneksi Modul ke jaringan WIFI
#include <ESP8266WiFi.h>
#include <Arduino.h>

// Include untuk mengetahui waktu pengambilan data secara realtime
#include "time.h"
#include <NTPClient.h>
#include <WiFiUdp.h>

// Include untuk mengaktifkan dan koneksikan Firebase agar terhubung dengan alat
#include <Firebase_ESP_Client.h>
#define EN 12
// Provide the token generation process info.
#include <addons/TokenHelper.h>

// Pin untuk relay
#define RELAY_PIN D4

/* Deklasi WIFI SSID dan Password ke ESP8622 */
#define WIFI_SSID "Paket Phoenix"
#define WIFI_PASSWORD "BacotKauBabi"

/* 2. Define the API Key */
#define API_KEY "AIzaSyCL9rnkHD9n3tD6_qIjfFODoWndQtKaz-4"
#define FIREBASE_PROJECT_ID "iotlistrik2"

/* 4. Define the user Email and password that alreadey registerd or added in your project */
#define USER_EMAIL "anangprayoga199@gmail.com"
#define USER_PASSWORD "Yoga24###"

// RTDB URL dan Auth untuk kontrol relay
#define FIREBASE_RTDB_URL "https://iotlistrik2-default-rtdb.asia-southeast1.firebasedatabase.app/"
#define FIREBASE_AUTH "AoXEx2zJuvg5enY6dcgpgBLueXhrSyCaESCoarze"

// Faktor kalibrasi untuk menyesuaikan pembacaan PZEM dengan digital wattmeter
#define ENERGY_CALIB_FACTOR 0.902  // Faktor kalibrasi (0.037/0.041 = 0.902)
#define POWER_CALIB_FACTOR 0.99    // Faktor kalibrasi untuk power jika diperlukan
#define CURRENT_CALIB_FACTOR 0.95  // Faktor kalibrasi untuk arus (0.182/0.193 = ~0.94)

// Define Firebase Data object
FirebaseData fbdo;
FirebaseData rtdbData; // Objek untuk Real-time Database
FirebaseAuth auth;
FirebaseConfig config;

unsigned long dataMillis = 0;
int count = 0;
int JumlahPerangkat;
bool relayState = true;
float previousPower = 0;
float powerThreshold = 20;

 
String weekDays[7]={"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

//Month names
String months[12]={"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};

// Define NTP Client to get time
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org");

// Variabel untuk melacak energi total yang sebenarnya (dikalibrasi)
float calibratedTotalEnergy = 0.0;
float lastRawEnergy = 0.0;
bool firstReading = true;

void fcsUploadCallback(CFS_UploadStatusInfo info)
{
    if (info.status == fb_esp_cfs_upload_status_init)
    {
        Serial.printf("\nUploading data (%d)...\n", info.size);
    }
    else if (info.status == fb_esp_cfs_upload_status_upload)
    {
        Serial.printf("Uploaded %d%s\n", (int)info.progress, "%");
    }
    else if (info.status == fb_esp_cfs_upload_status_complete)
    {
        Serial.println("Upload completed ");
    }
    else if (info.status == fb_esp_cfs_upload_status_process_response)
    {
        Serial.print("Processing the response... ");
    }
    else if (info.status == fb_esp_cfs_upload_status_error)
    {
        Serial.printf("Upload failed, %s\n", info.errorMsg.c_str());
    }
}

void setup()
{
    Serial.begin(9600);
    
    // Setup pin relay
    pinMode(RELAY_PIN, OUTPUT);
    digitalWrite(RELAY_PIN, !true); // Matikan relay pada awal (active LOW)
    
    // memulai koneksi WiFi pada sebuah perangkat
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    // membaca jaringan internet
    Serial.print("Connecting to Wi-Fi");
    unsigned long ms = millis();
    while (WiFi.status() != WL_CONNECTED)
    {
        Serial.print(".");
        delay(300);
    }
    // Mendapatkan local IP dari SSID
    Serial.println();
    Serial.print("Connected with IP: ");
    Serial.println(WiFi.localIP());
    Serial.println();

    Serial.printf("Firebase Client v%s\n\n", FIREBASE_CLIENT_VERSION);

    /*Tetapkan kunci api (wajib) */
    config.api_key = API_KEY;

    /* Assign the user sign in credentials */
    auth.user.email = USER_EMAIL;
    auth.user.password = USER_PASSWORD;

    // Konfigurasi RTDB untuk kontrol relay
    config.database_url = FIREBASE_RTDB_URL;
    config.signer.tokens.legacy_token = FIREBASE_AUTH;

    //get token
    /* Assign the callback function for the long running token generation task */
    config.token_status_callback = tokenStatusCallback; // see addons/TokenHelper.h
    
    // In ESP8266 required for BearSSL rx/tx buffer for large data handle, increase Rx size as needed.
    fbdo.setBSSLBufferSize(2048, 1024); // Reduced from 4096
    rtdbData.setBSSLBufferSize(1024, 512); // Reduced from 2048

    // Limit the size of response payload to be collected in FirebaseData
    fbdo.setResponseSize(2048);
    rtdbData.setResponseSize(1024);

    Firebase.begin(&config, &auth);

    Firebase.reconnectWiFi(true);
    
    // Buat child "relay_state" di Firebase RTDB jika belum ada
    if (Firebase.RTDB.getBool(&rtdbData, "/PZEM004T/relay_state")) {
        relayState = rtdbData.boolData();
        digitalWrite(RELAY_PIN, !relayState); // Terapkan status terakhir relay
        Serial.print("Status Relay awal: ");
        Serial.println(relayState ? "ON" : "OFF");
    } else {
        Firebase.RTDB.setBool(&rtdbData, "/PZEM004T/relay_state", false);
        Serial.println("Membuat node relay_state di RTDB");
    }
    
    // Mengambil nilai calibratedTotalEnergy yang tersimpan di Firebase
    if (Firebase.RTDB.getFloat(&rtdbData, "/PZEM004T/calibrated_energy")) {
        calibratedTotalEnergy = rtdbData.floatData();
        Serial.print("Mengambil nilai energi terkalibrasi dari Firebase: ");
        Serial.println(calibratedTotalEnergy, 3);
    } else {
        calibratedTotalEnergy = 0.0;
        Firebase.RTDB.setFloat(&rtdbData, "/PZEM004T/calibrated_energy", 0.0);
        Serial.println("Membuat node calibrated_energy di RTDB");
    }
    
    // Mengambil nilai lastRawEnergy yang tersimpan di Firebase
    if (Firebase.RTDB.getFloat(&rtdbData, "/PZEM004T/last_raw_energy")) {
        lastRawEnergy = rtdbData.floatData();
        firstReading = false;  // Karena sudah ada nilai terakhir
        Serial.print("Mengambil nilai raw energy terakhir dari Firebase: ");
        Serial.println(lastRawEnergy, 3);
    } else {
        lastRawEnergy = 0.0;
        firstReading = true;
        Firebase.RTDB.setFloat(&rtdbData, "/PZEM004T/last_raw_energy", 0.0);
        Serial.println("Membuat node last_raw_energy di RTDB");
    }
    
    // Initialize a NTPClient to get time
    timeClient.begin();
    // Set offset time in seconds to adjust for your timezone, for example:
    // GMT +1 = 3600
    // GMT +8 = 28800
    // GMT -1 = -3600
    // GMT 0 = 0
    timeClient.setTimeOffset(0);
}

//membuat dan mendapatkan name dengan char random
void getRandomStr(char* output, int len){
    char* eligible_chars = "AaBbCcDdEeFfGgHhIiJjKkLlMmNnOoPpQqRrSsTtUuVvWwXxYyZz1234567890";
    for(int i = 0; i< len; i++){
        uint8_t random_index = random(0, strlen(eligible_chars));
        output[i] = eligible_chars[random_index];
    }
    Serial.print("output: "); Serial.println(output);
}

// Fungsi untuk mengkonversi float ke format dengan jumlah desimal tertentu
float roundToDecimalPlaces(float value, int decimalPlaces) {
  float multiplier = pow(10.0, decimalPlaces);
  return round(value * multiplier) / multiplier;
}

// Fungsi untuk menghitung energi terkalibrasi dengan format yang konsisten
float getCalibratedEnergy(float rawEnergy) {
    if (firstReading) {
        lastRawEnergy = rawEnergy;
        firstReading = false;
        // Kalibrasi dan format ke 3 desimal
        calibratedTotalEnergy = roundToDecimalPlaces(rawEnergy * ENERGY_CALIB_FACTOR, 3);
        
        // Simpan nilai ke Firebase
        Firebase.RTDB.setFloat(&rtdbData, "/PZEM004T/last_raw_energy", lastRawEnergy);
        Firebase.RTDB.setFloat(&rtdbData, "/PZEM004T/calibrated_energy", calibratedTotalEnergy);
        
        return calibratedTotalEnergy;
    }
    
    // Hitung perbedaan energi sejak pembacaan terakhir
    float energyDiff = rawEnergy - lastRawEnergy;
    
    // Terapkan faktor kalibrasi hanya pada perbedaan (penambahan)
    if (energyDiff > 0) {
        calibratedTotalEnergy += energyDiff * ENERGY_CALIB_FACTOR;
        
        // Simpan nilai terbaru ke Firebase
        Firebase.RTDB.setFloat(&rtdbData, "/PZEM004T/calibrated_energy", calibratedTotalEnergy);
    }
    
    // Perbarui energi terakhir untuk pembacaan berikutnya
    lastRawEnergy = rawEnergy;
    Firebase.RTDB.setFloat(&rtdbData, "/PZEM004T/last_raw_energy", lastRawEnergy);
    
    // Format ke 3 desimal untuk menyesuaikan dengan digital wattmeter
    return roundToDecimalPlaces(calibratedTotalEnergy, 3);
}

void loop()
{
    //relay on pertama hidup
    if (Firebase.RTDB.getBool(&rtdbData, "/PZEM004T/relay_state")) {
        relayState = rtdbData.boolData();
        digitalWrite(RELAY_PIN, !relayState); // Terapkan status terakhir relay
        Serial.print("Status Relay awal: ");
        Serial.println(relayState ? "ON" : "OFF");
    } else {
        relayState = true; // Set to true by default
        Firebase.RTDB.setBool(&rtdbData, "/PZEM004T/relay_state", true); // Set to true in Firebase
        Serial.println("Membuat node relay_state di RTDB dengan status ON");
    }
    
    // Firebase.ready() should be called repeatedly to handle authentication tasks.
    if (Firebase.ready() && (millis() - dataMillis > 10000 || dataMillis == 0))
    {
        dataMillis = millis();

        // For the usage of FirebaseJson, see examples/FirebaseJson/BasicUsage/Create.ino
        FirebaseJson content;
        
        char iv_str[17] = {0}; //The last '\0' helps terminate the string
        getRandomStr(iv_str, 16);
        // We will create the nested document in the parent path "a0/b0/c0
        // a0 is the collection id, b0 is the document id in collection a0 and c0 is the collection id in the document b0.
        // and d? is the document id in the document collection id c0 which we will create.
        String documentPath = "DataBase1Jalur/d" + String(iv_str);
        // If the document path contains space e.g. "a b c/d e f"
        // It should encode the space as %20 then the path will be "a%20b%20c/d%20e%20f"

        //deklasi power dari pzem
        float rawPower = pzem.power();
        float power = roundToDecimalPlaces(rawPower * POWER_CALIB_FACTOR, 1); // Terapkan kalibrasi pada power dan bulatkan ke 1 desimal
        
        if(!isnan(rawPower)) {
            //Mendapatkan data power     
            Serial.print("Raw Power: "); Serial.print(rawPower); Serial.println("W");
            Serial.print("Calibrated Power: "); Serial.print(power); Serial.println("W");
            // Mengirimkan data power ke firebase             
            content.set("fields/power/doubleValue", power);
            // Also update power value in RTDB
            Firebase.RTDB.setFloat(&rtdbData, "/PZEM004T/power", power);
            Firebase.RTDB.setFloat(&rtdbData, "/PZEM004T/raw_power", rawPower); // Simpan juga nilai asli untuk perbandingan
        } else {
            //kondisi kita tidak mendapatkan power          
            Serial.println("Error reading power");
            content.set("fields/power/doubleValue", 0);
            Firebase.RTDB.setFloat(&rtdbData, "/PZEM004T/power", 0);
        }

        // To Mengetahui jumlah perangkat yang tercolok     
        float powerDifference = abs(power - previousPower);
        if (power == 0) {
            Serial.println("Perangkat tidak terhubung");
            JumlahPerangkat = 0;
        } else if (powerDifference > powerThreshold && power > previousPower) {
            // If power increased by more than the threshold, count a new device
            JumlahPerangkat++;
            Serial.print("Terdeteksi perangkat baru! Jumlah sekarang: ");
            Serial.println(JumlahPerangkat);
        } else if (powerDifference > powerThreshold && power < previousPower) {
            // If power decreased by more than the threshold, a device was disconnected
            JumlahPerangkat = max(0, JumlahPerangkat - 1);
            Serial.print("Perangkat dilepas! Jumlah sekarang: ");
            Serial.println(JumlahPerangkat);
        } else {
            Serial.print("Perangkat terhubung: ");
            Serial.println(JumlahPerangkat);
        }
        
        // Update previous power for next comparison
        previousPower = power;
        
        content.set("fields/JumlahPerangkat/doubleValue", JumlahPerangkat);

        // deklarasi untuk mendapatkan volt dari pzem
        float Tegangan = pzem.voltage();
        Tegangan = roundToDecimalPlaces(Tegangan, 1); // Bulatkan ke 1 desimal
        
        if(!isnan(Tegangan)) {
            //Mendapatkan data voltage dan menampilkan di serial monitor         
            Serial.print("Tegangan: "); Serial.print(Tegangan, 1); Serial.println("V");
            //mengirimkan data voltage ke firebase          
            content.set("fields/Tegangan/doubleValue", Tegangan);
            // Also update voltage in RTDB
            Firebase.RTDB.setFloat(&rtdbData, "/PZEM004T/voltage", Tegangan);
        } else {
            //kondisi ketika tidak mendapatkan volt
            Serial.println("Error reading Voltage");
            Firebase.RTDB.setFloat(&rtdbData, "/PZEM004T/voltage", 0);
        }
        
        // Deklarasi arus dari pzem
        float rawArus = pzem.current();
        float Arus = roundToDecimalPlaces(rawArus * CURRENT_CALIB_FACTOR, 3); // Terapkan kalibrasi pada arus dan bulatkan ke 3 desimal
        
        if(!isnan(rawArus)) {
            //mendapatkan data arus dan menampilkan data
            Serial.print("Raw Arus: "); Serial.print(rawArus, 3); Serial.println("A");
            Serial.print("Calibrated Arus: "); Serial.print(Arus, 3); Serial.println("A");
            // Mengirimkan data arus ke firebase           
            content.set("fields/Arus/doubleValue", Arus);
            // Also update current in RTDB
            Firebase.RTDB.setFloat(&rtdbData, "/PZEM004T/current", Arus);
            Firebase.RTDB.setFloat(&rtdbData, "/PZEM004T/raw_current", rawArus); // Simpan juga nilai asli
        } else {
            // kondisi ketika data tidak didapatkan
            Serial.println("Error reading Current");
            content.set("fields/Arus/doubleValue", 0);
            Firebase.RTDB.setFloat(&rtdbData, "/PZEM004T/current", 0);
        }
        
        //Deklarasi energi dari pzem
        float rawEnergy = pzem.energy();
        float energy = getCalibratedEnergy(rawEnergy); // Gunakan fungsi kalibrasi yang sudah diformat
        
        if(!isnan(rawEnergy)) {
            //mendapatkan data energi dan menampilkan data
            Serial.print("Raw Energy: "); Serial.print(rawEnergy, 3); Serial.println("kWh");
            Serial.print("Calibrated Energy: "); Serial.print(energy, 3); Serial.println("kWh");
            // Mengirimkan data energi ke firebase 
            content.set("fields/energy/doubleValue", energy);
            // Also update energy in RTDB
            Firebase.RTDB.setFloat(&rtdbData, "/PZEM004T/energy", energy);
            Firebase.RTDB.setFloat(&rtdbData, "/PZEM004T/raw_energy", rawEnergy); // Simpan juga nilai asli
        } else {
            // kondisi ketika data tidak didapatkan
            Serial.println("Error reading energy");
            content.set("fields/energy/doubleValue", 0);
            Firebase.RTDB.setFloat(&rtdbData, "/PZEM004T/energy", 0);
        }

        // Kirim status relay ke Firestore juga
        content.set("fields/relay_state/booleanValue", relayState);

        //  memanggil waktu secara update
        timeClient.update();

        // deklarasi untuk mengatur waktu
        time_t epochTime = timeClient.getEpochTime();

        // deklarasi untuk mendapatkan waktu 
        String formattedTime = timeClient.getFormattedTime();

        // Proses mereset energy sesuai dengan waktu ditentukan
        if(formattedTime >= "17:00:00" && formattedTime <= "17:01:00") {
            pzem.resetEnergy();
            // Reset juga energi terkalibrasi dan simpan ke Firebase
            calibratedTotalEnergy = 0.0;
            lastRawEnergy = 0.0;
            firstReading = true;
            
            // Simpan nilai reset ke Firebase
            Firebase.RTDB.setFloat(&rtdbData, "/PZEM004T/calibrated_energy", 0.0);
            Firebase.RTDB.setFloat(&rtdbData, "/PZEM004T/last_raw_energy", 0.0);
            
            Serial.println("Energy reset dilakukan!");
        }

        //mendapatkan dan menampilkan data sesuai req waktu
        if(formattedTime >= "16:50:00" && formattedTime <= "16:51:00") {
             content.set("fields/TotalEnergyDay/doubleValue", energy);
        }
        
        //Mendapatkan waktu dan mendeklarasikan
        Serial.print("Formatted Time: ");
        Serial.println(formattedTime);  
        
        String weekDay = weekDays[timeClient.getDay()];
        
        int Harga_listrik = energy * 1352;
        Serial.print("Harga listrik: RP. ");
        Serial.println(Harga_listrik);
        content.set("fields/HargaListrik/doubleValue", Harga_listrik);
        
        //Get a time structure
        struct tm *ptm = gmtime((time_t *)&epochTime); 
        
        int monthDay = ptm->tm_mday;
        int currentMonth = ptm->tm_mon+1;
        String currentMonthName = months[currentMonth-1];
        int currentYear = ptm->tm_year+1900;
        String currentDate = String(currentYear) + "-" + String(currentMonth) + "-" + String(monthDay);
        
        Serial.println("");
        //mengirimkan data waktu dan tanggal ke firebase
        content.set("fields/TimeStamp/timestampValue", currentDate + "T" + formattedTime + "Z");
        
        count++;
        //Menampilkan loading create document
        Serial.print("Create a document... ");
        //Untuk mengetahui ketika sudah terhubung dengan firebase 
        //membuat dokumen di firebase dengan data yang sudah didapatkan
        if (Firebase.Firestore.createDocument(&fbdo, FIREBASE_PROJECT_ID, "" /* databaseId can be (default) or empty */, documentPath.c_str(), content.raw()))
            Serial.println("Terhubung dengan Firebase");
        else
            Serial.println(fbdo.errorReason());
    }
    delay(30000);
}