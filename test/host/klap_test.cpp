// Drives the real TapoClient against mock_plug.py. Usage: klap_test <email> <password> <expect: ok|auth-fail>
#include <cstdlib>

#include "../../src/tapo_client.h"
#include "WiFi.h"

HostSerial Serial;
HostWiFi WiFi;

static int failures = 0;

static void check(bool cond, const char* what) {
    printf("%s %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) failures++;
}

int main(int argc, char** argv) {
    if (argc < 4) return 2;
    String expect = argv[3];

    TapoClient tapo;
    tapo.begin("127.0.0.1", argv[1], argv[2]);

    if (expect == "auth-fail") {
        check(!tapo.connect(), "connect is refused with wrong password");
        check(tapo.lastError().indexOf("credentials") >= 0 || tapo.lastError().indexOf("-1501") >= 0,
              "error message mentions credentials");
        return failures ? 1 : 0;
    }

    check(tapo.selfTest(), "crypto matches python-kasa reference vector");
    check(tapo.connect(), "KLAP handshake");
    check(tapo.getPower() == 0, "plug starts OFF");
    check(tapo.setPower(true), "turn ON");
    check(tapo.getPower() == 1, "plug reports ON");
    check(tapo.setPower(false), "turn OFF");
    check(tapo.getPower() == 0, "plug reports OFF");
    for (int i = 0; i < 5; i++) tapo.getPower();
    check(tapo.getPower() == 0, "still works after repeated requests / session expiry");
    return failures ? 1 : 0;
}
