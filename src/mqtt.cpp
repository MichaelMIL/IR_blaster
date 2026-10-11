#include "mqtt.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include "config.h"
#include "buttons.h"
#include "recordings.h"

static WiFiClient   net;
static PubSubClient client(net);
static String   devId, base, haPrefix = "homeassistant";
static uint32_t lastTry = 0;
static int      lastRc = 0;
static int      publishedButtons = 0;
static bool     everConnected = false;
static bool     suspended = false;

static String jsonEsc(const String &s) {
    String o;
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if (c == '"' || c == '\\') { o += '\\'; o += c; } else if ((uint8_t)c >= 0x20) o += c;
    }
    return o;
}

static String deviceJson() {
    return "\"device\":{\"identifiers\":[\"irb_" + devId + "\"],\"name\":\"IR Blaster\",\"model\":\"CB3S (BK7231N)\","
           "\"manufacturer\":\"ir_blaster\",\"sw_version\":\"LibreTiny " + String(lt_get_version()) + "\"}";
}

static void pub(const String &topic, const String &payload, bool retain) {
    if (!client.connected()) return;
    client.publish(topic.c_str(), payload.c_str(), retain);
}

void mqttPublishDiscovery() {
    if (!client.connected()) return;
    String avail = ",\"availability_topic\":\"" + base + "/status\"";
    int n = btnCount();
    for (int i = 0; i < n; i++) {
        const Button *b = btnGet(i);
        String topic = haPrefix + "/button/irb_" + devId + "/b" + String(i) + "/config";
        String p = "{\"name\":\"" + jsonEsc(b->label) + "\",\"unique_id\":\"irb_" + devId + "_b" + String(i) + "\","
                   "\"command_topic\":\"" + base + "/cmd/button\",\"payload_press\":\"" + String(i) + "\","
                   "\"icon\":\"mdi:remote\"" + avail + "," + deviceJson() + "}";
        pub(topic, p, true);
    }
    for (int i = n; i < publishedButtons; i++)                       // remove deleted buttons
        pub(haPrefix + "/button/irb_" + devId + "/b" + String(i) + "/config", "", true);
    publishedButtons = n;

    String p = "{\"name\":\"Last IR code\",\"unique_id\":\"irb_" + devId + "_received\","
               "\"state_topic\":\"" + base + "/received\",\"value_template\":\"{{ value_json.code }}\","
               "\"json_attributes_topic\":\"" + base + "/received\",\"icon\":\"mdi:remote-tv\"" + avail + "," + deviceJson() + "}";
    pub(haPrefix + "/sensor/irb_" + devId + "/received/config", p, true);
}

void mqttPublishReceived(const RxRecord &r) {
    if (!client.connected()) return;
    char code[24], raw[12];
    if (r.type == 1)      snprintf(code, sizeof(code), "NEC %02X/%02X", r.address, r.command);
    else if (r.type == 2) snprintf(code, sizeof(code), "NEC repeat");
    else                  snprintf(code, sizeof(code), "raw %u pulses", r.pulses);
    snprintf(raw, sizeof(raw), "0x%08lX", (unsigned long)r.raw);
    String p = "{\"code\":\"" + String(code) + "\",\"type\":\"" + (r.type == 1 ? "nec" : r.type == 2 ? "repeat" : "raw") + "\"";
    if (r.type == 1) p += ",\"addr\":" + String(r.address) + ",\"cmd\":" + String(r.command) + ",\"raw\":\"" + raw + "\"";
    p += ",\"pulses\":" + String(r.pulses) + ",\"uptime\":" + String(millis() / 1000) + "}";
    pub(base + "/received", p, false);
}

static void onMessage(char *topic, uint8_t *payload, unsigned int len) {
    String t(topic), p;
    for (unsigned i = 0; i < len; i++) p += (char)payload[i];
    p.trim();
    String cmd = t.substring(base.length() + 5);          // after "<base>/cmd/"
    String msg; bool ok = false;
    if (cmd == "button") {
        int idx = -1;
        if (p.length() && isDigit(p[0])) idx = p.toInt();
        else for (int i = 0; i < btnCount(); i++) if (p.equalsIgnoreCase(btnGet(i)->label)) { idx = i; break; }
        ok = appPressButton(idx, msg);
    } else if (cmd == "send") {
        unsigned a = 0, c = 0; int got = 0;
        int ia = p.indexOf("\"addr\""), ic = p.indexOf("\"cmd\"");
        if (ia >= 0 && ic >= 0) {
            String sa = p.substring(p.indexOf(':', ia) + 1), sc = p.substring(p.indexOf(':', ic) + 1);
            sa.replace("\"", " "); sc.replace("\"", " ");
            got = (sscanf(sa.c_str(), "%i", &a) == 1) + (sscanf(sc.c_str(), "%i", &c) == 1);
        } else {
            String q = p; q.replace(",", " "); q.replace("/", " ");
            got = sscanf(q.c_str(), "%i %i", &a, &c);
        }
        if (got == 2) { ok = appSendNec(a, c); msg = ok ? "sent" : "no LED pins"; } else msg = "bad payload";
    } else if (cmd == "raw") {
        ok = appSendRaw(strtoul(p.c_str(), nullptr, 0)); msg = ok ? "sent" : "no LED pins";
    } else if (cmd == "play") {
        ok = appPlay(p.c_str(), msg);
    } else {
        msg = "unknown command topic";
    }
    Serial.printf("mqtt: %s '%s' -> %s\n", cmd.c_str(), p.c_str(), msg.c_str());
    pub(base + "/result", String("{\"ok\":") + (ok ? "true" : "false") + ",\"message\":\"" + jsonEsc(msg) + "\"}", false);
}

void mqttReconfigure() {
    if (client.connected()) client.disconnect();
    lastTry = 0;
}

void mqttBegin() {
    String mac = WiFi.macAddress();            // "AA:BB:CC:DD:EE:FF"
    mac.replace(":", ""); mac.toLowerCase();
    devId = mac.length() >= 6 ? mac.substring(mac.length() - 6) : "000000";
    base  = "ir_blaster/" + devId;
    client.setBufferSize(1024);
    client.setCallback(onMessage);
    client.setKeepAlive(30);
}

void mqttSuspend() { suspended = true; if (client.connected()) client.disconnect(); client.setBufferSize(64); }
void mqttResume()  { suspended = false; lastTry = 0; client.setBufferSize(1024); }

void mqttLoop() {
    if (suspended || !cfg.mqttHost.length() || WiFi.status() != WL_CONNECTED) return;
    if (!client.connected()) {
        if (millis() - lastTry < 5000) return;
        lastTry = millis();
        client.setServer(cfg.mqttHost.c_str(), cfg.mqttPort);
        String cid = "ir-blaster-" + devId;
        String will = base + "/status";
        bool okc = client.connect(cid.c_str(),
                                  cfg.mqttUser.length() ? cfg.mqttUser.c_str() : nullptr,
                                  cfg.mqttUser.length() ? cfg.mqttPass.c_str() : nullptr,
                                  will.c_str(), 0, true, "offline");
        lastRc = client.state();
        if (!okc) { Serial.printf("mqtt: connect to %s:%u failed (rc=%d)\n", cfg.mqttHost.c_str(), cfg.mqttPort, lastRc); return; }
        everConnected = true;
        Serial.printf("mqtt: connected to %s:%u as %s, base topic %s\n", cfg.mqttHost.c_str(), cfg.mqttPort, cid.c_str(), base.c_str());
        client.publish(will.c_str(), "online", true);
        client.subscribe((base + "/cmd/#").c_str());
        publishedButtons = BTN_MAX;            // clear any stale discovery entries once
        mqttPublishDiscovery();
    }
    client.loop();
}

String mqttState() {
    if (!cfg.mqttHost.length()) return "disabled";
    if (suspended) return "suspended (update in progress)";
    if (client.connected()) return "connected";
    if (WiFi.status() != WL_CONNECTED) return "waiting for wifi";
    if (lastRc == 0 && !everConnected) return "connecting";
    switch (lastRc) {
        case -2: return "error: connection refused (host/port?)";
        case -4: return "error: timeout";
        case 4:  return "error: bad username/password";
        case 5:  return "error: not authorised";
        default: return "error rc=" + String(lastRc) + ", reconnecting";
    }
}

String mqttBaseTopic() { return base; }
