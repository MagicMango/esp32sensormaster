#include <WiFiClient.h>
#include <ArduinoJson.h>
#include <FS.h>
#include "SPIFFS.h"
#include "ESPAsyncWebServer.h"
#include "esp_wifi.h"
#include <HTTPClient.h>
#include "Sensor.h"
#include "SensorData.h"
#include "Configuration.h"

extern "C" {
#include <ctime>
}

bool parseSensorJSON();
String macToString(uint8_t mac[]);
void handleRoot(AsyncWebServerRequest *request);
void handleWebRequests(AsyncWebServerRequest *request);
bool loadFromSpiffs(AsyncWebServerRequest *request, String path);
SensorInfo parseSensorInfo(String JSONMessage);
SensorData parseSensorData(String JSONMessage, String v);
void saveSensorSettings();
void StationGotIp(WiFiEvent_t event, WiFiEventInfo_t info);
void WifiDisconnected(WiFiEvent_t event, WiFiEventInfo_t info);
String getTime();

static Configuration conf;
//10 Seconds
static Sensor sensors[8];
const String dataFolder = "/sensordata/";
static StaticJsonBuffer<JSON_ARRAY_SIZE(8) + JSON_OBJECT_SIZE(8 * 4)> jsonBuffer;
static StaticJsonBuffer<JSON_ARRAY_SIZE(8) + JSON_OBJECT_SIZE(8 * 4)> sensorBuffer;


AsyncWebServer server(80);

void setup() {
	Serial.begin(115200);
	//Initialize File System
	SPIFFS.begin(true);
	Serial.println();
	if (readConfig()) {
		if (parseSensorJSON()) {
			Serial.println(F("devices.json loaded."));
		}
		initWifi();
		initWebserver();
	}
	else {
		Serial.println(F("Config not readable"));
	}
}
void loop() {
	delay(1);
	if (ESP.getFreeHeap() <= 10000) {
		ESP.restart();
	}
}

String getTime()
{
	struct tm timeinfo;
	char buffer[80];
	if (!getLocalTime(&timeinfo)) {
		Serial.println(F("Failed to obtain time"));
		return "";
	}
	strftime(buffer, 80,"%Y-%m-%d %H:%M:%S", &timeinfo);
	return String(buffer);
}

void initWifi() {
	char ssid[conf.ssid.length() + 1];
	char password[conf.password.length() + 1];
	conf.ssid.toCharArray(ssid, conf.ssid.length() + 1);
	conf.password.toCharArray(password, conf.password.length() + 1);
	WiFi.mode(WIFI_AP_STA);
	WiFi.begin(ssid, password);     

	while (WiFi.status() != WL_CONNECTED) {
		delay(500);
		Serial.print(F("."));
	}

	Serial.println("");
	Serial.print(F("Connected to "));
	Serial.println(ssid);
	Serial.print(F("IP address: "));
	Serial.println(WiFi.localIP());  
	configTime(3600, 3600,"de.pool.ntp.org");
	Serial.println(getTime());
	if (WiFi.softAP("ESP32Master", "[a secure password]", 6, false, 8)) {
		Serial.println(F("AP Started."));
		Serial.print(F("APIP: "));
		Serial.print(WiFi.softAPIP());
		Serial.println();
		WiFi.onEvent(StationGotIp, WiFiEvent_t::SYSTEM_EVENT_AP_STAIPASSIGNED);
		WiFi.onEvent(WifiDisconnected, WiFiEvent_t::SYSTEM_EVENT_AP_STADISCONNECTED);
	}
	else {
		Serial.println(F("AP not established."));
	}
}
void initWebserver() {
	server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
		handleRoot(request);
	});
	server.on("/getStatistics", HTTP_GET, [](AsyncWebServerRequest *request) {
		getStatistics(request);
	});
	server.onNotFound([](AsyncWebServerRequest *request) {
		handleWebRequests(request);
	});
	//TODO: finish
	server.on("/upload", HTTP_GET, [](AsyncWebServerRequest *request) {                 // if the client requests the upload page
		request->send(200, "text/html", "<form method=\"post\" enctype=\"multipart/form-data\"><input type=\"file\" name=\"name\"><input class=\"button\" type=\"submit\" value=\"Upload\"></form>");
	});
	server.begin();
	Serial.println("HTTP server started");

}
bool readConfig() {
	size_t size = 0;
	File f = SPIFFS.open("/config.json", "r");
	if (!f) {
		Serial.println(F("Could not open config.json"));
		return false;
	}
	else
	{
		size = f.size();
		std::unique_ptr<char[]> buf(new char[size]);
		f.readBytes(buf.get(), size);
		JsonObject& parsed = jsonBuffer.parseObject(buf.get());
		if (!parsed.success()) {
			return false;
		}
		jsonBuffer.clear();
		conf = { parsed["ssid"], parsed["password"] };
		return true;
	}
}
void periodicUpdate() {
	SensorData result;
	File f;
	for (int i = 0; i < sizeof(sensors) / sizeof(sensors[0]); i++) {
		if (sensors[i].SensorIP != "") {
			Serial.println("http://" + sensors[i].SensorIP + "/" + sensors[i].SensorFunction);
			result = getSensorData(sensors[i].SensorIP, sensors[i].SensorFunction, sensors[i].FunctionVersion);
			f = SPIFFS.open(dataFolder + sensors[i].SensorMAC, "a");
			if (!f) {
				Serial.println(F("Could not write sensor data."));
			}
			f.println(getTime() + " " + String(result.Temperature) + " " + String(result.Humidity) + " " + String(result.Pressure));
			Serial.println(getTime() + " " + String(result.Temperature) + " " + String(result.Humidity) + " " + String(result.Pressure));
			f.close();
			sensors[i].SensorIP = "";
			delay(0);
		}
	}
	Serial.println("Free Heap: " + String(ESP.getFreeHeap()));
}
void StationGotIp(WiFiEvent_t e, WiFiEventInfo_t info)
{
	wifi_sta_list_t wifi_ap_list;
	tcpip_adapter_sta_list_t tcp_sta_list;
	esp_wifi_ap_get_sta_list(&wifi_ap_list);
	tcpip_adapter_get_sta_list(&wifi_ap_list, &tcp_sta_list);
	for (size_t i = 0; i < wifi_ap_list.num; i++)
	{
		SensorInfo sinfo;
		IPAddress connected = IPAddress(tcp_sta_list.sta[i].ip.addr);
		bool found = false;
		Serial.println(F("Found MAC, search for empty sensor space or known sensor."));
		for (int i = 0; i < sizeof(sensors) / sizeof(sensors[0]); i++) {
			if (!sensors[i].isInitialised()) {
				Serial.print(F("Will create new Sensor with IP: "));
				Serial.print(connected.toString());
				Serial.println(F(""));
				sinfo = getSensorInfo(connected.toString());
				sensors[i] = { connected.toString(), macToString(wifi_ap_list.sta[i].mac), sinfo.Function, sinfo.Version };
				found = true;
				break;
			}
			else if (sensors[i].SensorMAC == macToString(wifi_ap_list.sta[i].mac)) {
				Serial.println(F("Found known Sensor updating values."));
				sinfo = getSensorInfo(connected.toString());
				sensors[i] = { connected.toString(), macToString(wifi_ap_list.sta[i].mac), sinfo.Function, sinfo.Version };
				found = true;
				break;
			}
		}
		if (!found)Serial.println(F("Maximum Sensors rechaed."));
	}
	saveSensorSettings();
}
void WifiDisconnected(WiFiEvent_t event, WiFiEventInfo_t info) {
	String lastMAC = macToString(info.disconnected.bssid);
	if (lastMAC != "") {
		for (int i = 0; i < sizeof(sensors) / sizeof(sensors[0]); i++) {
			if (sensors[i].SensorMAC == lastMAC) {
				Serial.println("Sensor with MAC: " + lastMAC + " disconnected. Clearing IP");
				sensors[i].SensorIP = "";
			}
		}
	}
	saveSensorSettings();
}
String macToString(uint8_t  mac[]) {
	char buf[20];
	snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
		mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	return String(buf);
}
void getStatistics(AsyncWebServerRequest *request) {
	if (request->args() == 1) {
		if (request->argName(0) == "MAC") {
			if (!loadFromSpiffs(request, dataFolder + request->arg("MAC")))request->send(404, "text/plain", "404 Not Found");
		}
	}
}
/**
Json related Functions start
*/
SensorData getSensorData(String ip, String function, String versionf) {
	HTTPClient http;
	http.begin("http://" + ip + "/" + function);
	int httpCode = http.GET();
	SensorData result{ -1, -1, -2 };
	if (httpCode > 0) {
		String payload = http.getString();   //Get the request response payload
		result = parseSensorData(payload, versionf);
		delay(0);
	}
	http.end();
	return result;
}
SensorData parseSensorData(String JSONMessage, String v) {
	JsonObject& parsed = jsonBuffer.parseObject(JSONMessage); //Parse message
	if (!parsed.success()) {   //Check for errors in parsing
		return SensorData{ -1, -2, -2 };
	}
	jsonBuffer.clear();
	if (v == "v1") {
		return SensorData{ parsed["Intern"]["temperature"], parsed["Intern"]["humidity"], -1 };
	}
	else if (v == "v2") {
		return SensorData{ parsed["Intern"]["temperature"], parsed["Intern"]["humidity"], parsed["Intern"]["pressure"] };
	}
	else {
		return SensorData{ -2, -2, -2 };
	}
}
SensorInfo getSensorInfo(String ip) {
	HTTPClient http;
	http.begin("http://" + ip + "/getSensorInfo");
	int httpCode = http.GET();
	SensorInfo result;
	while (httpCode < 0) {
		httpCode = http.GET();
	}
	String payload = http.getString();   //Get the request response payload
	result = parseSensorInfo(payload);
	http.end();
	delay(0);
	Serial.println(payload);
	return result;
}
SensorInfo parseSensorInfo(String JSONMessage) {
	JsonObject& parsed = jsonBuffer.parseObject(JSONMessage); //Parse message
	if (!parsed.success()) {   //Check for errors in parsing
		return SensorInfo{ "", "" };
	}
	jsonBuffer.clear();
	return SensorInfo{ parsed["function"], parsed["version"] };
}
bool parseSensorJSON() {
	size_t size = 0;
	File f = SPIFFS.open("/devices.json", "r");
	if (!f) {
		Serial.println(F("Could not open devices.json"));
		return false;
	}
	else
	{
		size = f.size();
		std::unique_ptr<char[]> buf(new char[size]);
		f.readBytes(buf.get(), size);
		JsonArray& parsed = jsonBuffer.parseArray(buf.get()); //Parse message
		if (!parsed.success()) {   //Check for errors in parsing
			Serial.println(F("Error parsing sensors"));
			return false;
		}
		int i = 0;
		for (auto& sensor : parsed) {
			sensors[i] = { "", sensor["SensorMAC"], sensor["SensorFunction"], sensor["FunctionVersion"] };
			i++;
		}
		jsonBuffer.clear();
		delay(0);
		return true;
	}

}
void saveSensorSettings() {
	JsonArray& jsonSensors = sensorBuffer.createArray();
	File f = SPIFFS.open("/devices.json", "w");
	if (f) {
		for (int i = 0; i < sizeof(sensors) / sizeof(sensors[0]); i++) {
			if (sensors[i].isInitialised()) {
				JsonObject& jsonSensor = sensorBuffer.createObject();
				jsonSensor["SensorIP"] = sensors[i].SensorIP;
				jsonSensor["SensorMAC"] = sensors[i].SensorMAC;
				jsonSensor["SensorFunction"] = sensors[i].SensorFunction;
				jsonSensor["FunctionVersion"] = sensors[i].FunctionVersion;
				jsonSensors.add(jsonSensor);
				delay(0);
			}
		}
		jsonSensors.printTo(f);
		f.close();
	}
	sensorBuffer.clear();
}
void handleRoot(AsyncWebServerRequest *request) {
	request->redirect("/index.html");
}
void handleWebRequests(AsyncWebServerRequest *request) {
	if (loadFromSpiffs(request, "")) return;
	request->send(404, "text/plain", "404 Not Found");
}
bool loadFromSpiffs(AsyncWebServerRequest *request, String expath = "") {
	String path;
	if (expath != "") {
		path = expath;
	}
	else {
		path = request->url();
	}
	if (!SPIFFS.exists(path.c_str()) || path.endsWith("config.json")) { return false; }
	String dataType = "text/plain";
	if (!path.startsWith("/")) path = "/" + path;
	if (path.endsWith("/")) path += "index.html";
	if (path.endsWith(".src")) path = path.substring(0, path.lastIndexOf("."));
	else if (path.endsWith(".html")) dataType = "text/html";
	else if (path.endsWith(".htm")) dataType = "text/html";
	else if (path.endsWith(".css")) dataType = "text/css";
	else if (path.endsWith(".js")) dataType = "application/javascript";
	else if (path.endsWith(".png")) dataType = "image/png";
	else if (path.endsWith(".gif")) dataType = "image/gif";
	else if (path.endsWith(".jpg")) dataType = "image/jpeg";
	else if (path.endsWith(".ico")) dataType = "image/x-icon";
	else if (path.endsWith(".xml")) dataType = "text/xml";
	else if (path.endsWith(".pdf")) dataType = "application/pdf";
	else if (path.endsWith(".zip")) dataType = "application/zip";
	if (request->hasArg("download")) dataType = "application/octet-stream";
	request->send(SPIFFS, path, dataType);
	return true;
}
