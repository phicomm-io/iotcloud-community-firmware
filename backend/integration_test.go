package main

import (
	mqtt "github.com/eclipse/paho.mqtt.golang"
	"os"
	"testing"
	"time"
)

func TestRealDynamicSecurity(t *testing.T) {
	runtime := os.Getenv("PHICOMM_TEST_RUNTIME")
	if runtime == "" {
		t.Skip("isolated broker required")
	}
	pass, e := readSecret(runtime + "/secrets/broker-password")
	if e != nil {
		t.Fatal(e)
	}
	b, e := newDynamicBroker(os.Getenv("PHICOMM_TEST_BROKER"), runtime+"/broker/certs/ca.crt", serviceUser, pass)
	if e != nil {
		t.Fatal(e)
	}
	defer b.Close()
	deadline := time.Now().Add(10 * time.Second)
	for !b.client.IsConnectionOpen() && time.Now().Before(deadline) {
		time.Sleep(50 * time.Millisecond)
	}
	s, e := openStore(runtime + "/integration.db")
	if e != nil {
		t.Fatal(e)
	}
	defer s.Close()
	a := &Application{store: s, broker: b}
	u := "test_" + randomID()[:12]
	v := "test_" + randomID()[:12]
	pw := "integration-password"
	if e = a.register(u, pw); e != nil {
		t.Fatal("register", e)
	}
	if e = a.register(v, pw); e != nil {
		t.Fatal(e)
	}
	defer a.deleteUser(u)
	defer a.deleteUser(v)
	if e = b.Authenticate(u, "wrongpassword"); e != errCredentials {
		t.Fatal("bad password", e)
	}
	if e = b.Authenticate(u, pw); e != nil {
		t.Fatal("auth", e)
	}
	d := Device{MAC: "aa1122334455", Model: "m1"}
	if e = a.saveDevice(u, d, false); e != nil {
		t.Fatal("bind", e)
	}
	connect := func(user string) mqtt.Client {
		opts := mqtt.NewClientOptions().AddBroker(b.url).SetTLSConfig(b.tls).SetUsername(user).SetPassword(pw).SetClientID("test_" + randomID()[:16]).SetAutoReconnect(false)
		c := mqtt.NewClient(opts)
		if e := waitToken(c.Connect()); e != nil {
			t.Fatal(e)
		}
		t.Cleanup(func() { c.Disconnect(0) })
		return c
	}
	own, other := connect(u), connect(v)
	grant := func(c mqtt.Client, topic string) byte {
		tok := c.Subscribe(topic, 1, nil)
		if !tok.WaitTimeout(5 * time.Second) {
			t.Fatal("subscribe timeout")
		}
		if tok.Error() != nil {
			return 128
		}
		return tok.(*mqtt.SubscribeToken).Result()[topic]
	}
	if g := grant(own, "phicomm/m1/aa1122334455/#"); g == 128 {
		t.Fatal("owner denied")
	}
	if g := grant(other, "phicomm/m1/aa1122334455/#"); g != 128 {
		t.Fatal("other allowed", g)
	}
	if g := grant(own, "$CONTROL/dynamic-security/v1/response"); g != 128 {
		t.Fatal("control accessible", g)
	}
	if g := grant(own, "phicomm/a1/aa1122334455/#"); g != 128 {
		t.Fatal("model accessible", g)
	}
	received := make(chan string, 1)
	tok := own.Subscribe("phicomm/m1/aa1122334455/brightness/set", 1, func(_ mqtt.Client, m mqtt.Message) { received <- string(m.Payload()) })
	if e = waitToken(tok); e != nil {
		t.Fatal(e)
	}
	if e = waitToken(own.Publish("phicomm/m1/aa1122334455/brightness/set", 1, false, "0")); e != nil {
		t.Fatal(e)
	}
	select {
	case value := <-received:
		if value != "0" {
			t.Fatal(value)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("owner command not delivered")
	}
	if e = a.removeDevice(u, "aa1122334455"); e != nil {
		t.Fatal("unbind", e)
	}
	own.Disconnect(0)
	own = connect(u)
	if grant(own, "phicomm/m1/aa1122334455/#") != 128 {
		t.Fatal("permissions retained")
	}
	if e = a.saveDevice(v, d, false); e != nil {
		t.Fatal("rebind", e)
	}
	if e = a.changePassword(u, "replacement-password"); e != nil {
		t.Fatal(e)
	}
	if e = b.Authenticate(u, pw); e != errCredentials {
		t.Fatal("old password accepted", e)
	}
	if e = b.Authenticate(u, "replacement-password"); e != nil {
		t.Fatal("new password", e)
	}
}

func TestRealPersistence(t *testing.T) {
	runtime := os.Getenv("PHICOMM_TEST_RUNTIME")
	phase := os.Getenv("PHICOMM_TEST_PHASE")
	if runtime == "" || phase == "" {
		t.Skip("isolated restart phase required")
	}
	pass, e := readSecret(runtime + "/secrets/broker-password")
	if e != nil {
		t.Fatal(e)
	}
	b, e := newDynamicBroker(os.Getenv("PHICOMM_TEST_BROKER"), runtime+"/broker/certs/ca.crt", serviceUser, pass)
	if e != nil {
		t.Fatal(e)
	}
	defer b.Close()
	deadline := time.Now().Add(10 * time.Second)
	for !b.client.IsConnectionOpen() && time.Now().Before(deadline) {
		time.Sleep(50 * time.Millisecond)
	}
	s, e := openStore(runtime + "/restart.db")
	if e != nil {
		t.Fatal(e)
	}
	defer s.Close()
	a := &Application{store: s, broker: b}
	const user = "restart_test"
	const password = "restart-test-password"
	if phase == "seed" {
		if e = a.register(user, password); e != nil {
			t.Fatal(e)
		}
		for i, model := range []string{"tc1", "m1", "a1"} {
			d := Device{MAC: []string{"aa1111111111", "aa2222222222", "aa3333333333"}[i], Model: model}
			if e = a.saveDevice(user, d, false); e != nil {
				t.Fatal(e)
			}
		}
		return
	}
	if e = b.Authenticate(user, password); e != nil {
		t.Fatal("password not persisted", e)
	}
	ds, e := s.devices(user)
	if e != nil || len(ds) != 3 {
		t.Fatal("database not persisted", ds, e)
	}
	data, e := b.Command("getClient", map[string]interface{}{"username": user})
	if e != nil {
		t.Fatal(e)
	}
	for _, d := range ds {
		if !hasRole(data, "device_"+d.MAC) {
			t.Fatal("role not persisted", d)
		}
	}
	if e = a.deleteUser(user); e != nil {
		t.Fatal(e)
	}
	if e = b.Authenticate(user, password); e != errCredentials {
		t.Fatal("deleted user can authenticate", e)
	}
}

func TestRealMigration(t *testing.T) {
	runtime := os.Getenv("PHICOMM_TEST_RUNTIME")
	if runtime == "" || os.Getenv("PHICOMM_TEST_MIGRATED") != "1" {
		t.Skip("isolated migrated broker required")
	}
	pass, e := readSecret(runtime + "/secrets/broker-password")
	if e != nil {
		t.Fatal(e)
	}
	b, e := newDynamicBroker(os.Getenv("PHICOMM_TEST_BROKER"), runtime+"/broker/certs/ca.crt", serviceUser, pass)
	if e != nil {
		t.Fatal(e)
	}
	defer b.Close()
	deadline := time.Now().Add(15 * time.Second)
	for !b.client.IsConnectionOpen() && time.Now().Before(deadline) {
		time.Sleep(50 * time.Millisecond)
	}
	if e = b.Authenticate("legacy_test", "original-test-password"); e != nil {
		t.Fatal("legacy password changed", e)
	}
	s, e := openStore(runtime + "/app/app.db")
	if e != nil {
		t.Fatal(e)
	}
	defer s.Close()
	ds, e := s.devices("legacy_test")
	if e != nil || len(ds) != 1 || ds[0].MAC != "aabbccddeeff" {
		t.Fatal(ds, e)
	}
	data, e := b.Command("getClient", map[string]interface{}{"username": "legacy_test"})
	if e != nil || !hasRole(data, "device_aabbccddeeff") {
		t.Fatal("migrated role missing", e)
	}
}
