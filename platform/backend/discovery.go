package main

import (
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"errors"
	"net/http"
	"strings"
)

func managerRules() []Rule {
	return []Rule{
		{"publishClientSend", "$CONTROL/dynamic-security/v1", true, 1},
		{"publishClientReceive", "$CONTROL/dynamic-security/v1/response", true, 1},
		{"subscribeLiteral", "$CONTROL/dynamic-security/v1/response", true, 1},
		{"publishClientReceive", "phicomm/discovery/+/+", true, 1},
		{"subscribePattern", "phicomm/discovery/+/+", true, 1},
	}
}

// Encoding is bijective and never introduces MQTT wildcard/path characters.
func userKey(username string) string { return "u_" + hex.EncodeToString([]byte(username)) }
func deviceRole(d Device) string {
	if d.Namespace != "" {
		return "scoped_" + userKey(d.Username) + "_" + d.MAC
	}
	return "device_" + d.MAC
}
func scopeRules(d Device) []Rule {
	topic := "phicomm/" + d.Model + "/" + userKey(d.Username) + "/" + d.MAC + "/#"
	return []Rule{{"publishClientSend", topic, true, 1}, {"publishClientReceive", topic, true, 1}, {"subscribePattern", topic, true, 1}}
}
func (a *Application) syncAccount(username string) error {
	role := "account_" + userKey(username)
	rules := []Rule{{"publishClientSend", "phicomm/discovery/" + userKey(username) + "/+", true, 1}}
	for _, topic := range []string{"homeassistant/+/" + userKey(username) + "/#", "phicomm/discovery/" + userKey(username) + "/+"} {
		for _, kind := range []string{"publishClientReceive", "subscribePattern"} {
			rules = append(rules, Rule{kind, topic, true, 1})
		}
	}
	rules = append(rules, Rule{"publishClientSend", "homeassistant/+/" + userKey(username) + "/#", true, 1}, Rule{"subscribeLiteral", "homeassistant/status", true, 1}, Rule{"publishClientReceive", "homeassistant/status", true, 1})
	_, err := a.broker.Command("getRole", map[string]interface{}{"rolename": role})
	if notFound(err) {
		_, err = a.broker.Command("createRole", map[string]interface{}{"rolename": role, "acls": rules})
	}
	if err != nil {
		return err
	}
	data, err := a.broker.Command("getClient", map[string]interface{}{"username": username})
	if err != nil {
		return err
	}
	if !hasRole(data, role) {
		_, err = a.broker.Command("addClientRole", map[string]interface{}{"username": username, "rolename": role})
	}
	return err
}
func (a *Application) syncAccounts() error {
	rows, err := a.store.Query("SELECT username FROM users WHERE state='active'")
	if err != nil {
		return err
	}
	var users []string
	for rows.Next() {
		var u string
		if err = rows.Scan(&u); err != nil {
			rows.Close()
			return err
		}
		users = append(users, u)
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return err
	}
	for _, u := range users {
		if err = a.syncAccount(u); err != nil {
			return err
		}
	}
	return nil
}
func (s *Store) scopedDevices(username string) ([]Device, error) {
	q := "SELECT username,mac,model,version,status FROM scoped_devices WHERE status!='hidden'"
	args := []interface{}{}
	if username != "" {
		q += " AND username=?"
		args = append(args, username)
	}
	rows, err := s.Query(q, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	list := []Device{}
	for rows.Next() {
		d := Device{Namespace: "account"}
		if err = rows.Scan(&d.Username, &d.MAC, &d.Model, &d.Version, &d.Status); err != nil {
			return nil, err
		}
		list = append(list, d)
	}
	return list, rows.Err()
}
func (a *Application) syncScopedDevice(d Device) error {
	role := deviceRole(d)
	if d.Status == "deleting" {
		_, err := a.broker.Command("removeClientRole", map[string]interface{}{"username": d.Username, "rolename": role})
		if err != nil && !notFound(err) {
			return err
		}
		_, err = a.broker.Command("deleteRole", map[string]interface{}{"rolename": role})
		if err != nil && !notFound(err) {
			return err
		}
		_, err = a.store.Exec("UPDATE scoped_devices SET status='hidden' WHERE username=? AND mac=?", d.Username, d.MAC)
		return err
	}
	_, err := a.broker.Command("getRole", map[string]interface{}{"rolename": role})
	if notFound(err) {
		_, err = a.broker.Command("createRole", map[string]interface{}{"rolename": role, "acls": scopeRules(d)})
	} else if err == nil {
		_, err = a.broker.Command("modifyRole", map[string]interface{}{"rolename": role, "acls": scopeRules(d)})
	}
	if err != nil {
		return err
	}
	data, err := a.broker.Command("getClient", map[string]interface{}{"username": d.Username})
	if err != nil {
		return err
	}
	if !hasRole(data, role) {
		_, err = a.broker.Command("addClientRole", map[string]interface{}{"username": d.Username, "rolename": role})
	}
	if err != nil {
		return err
	}
	_, err = a.store.Exec("UPDATE scoped_devices SET status='active' WHERE username=? AND mac=?", d.Username, d.MAC)
	return err
}
func (a *Application) discover(topic string, payload []byte) error {
	parts := strings.Split(topic, "/")
	if len(parts) != 4 || parts[0] != "phicomm" || parts[1] != "discovery" || !strings.HasPrefix(parts[2], "u_") || len(payload) == 0 || len(payload) > 4096 {
		return errors.New("invalid discovery")
	}
	decoded, err := hex.DecodeString(strings.TrimPrefix(parts[2], "u_"))
	if err != nil {
		return err
	}
	username := string(decoded)
	if userKey(username) != parts[2] || !usernameRegex.MatchString(username) || reservedUser(username) {
		return errors.New("invalid owner")
	}
	var announcement struct {
		Schema  int    `json:"schema"`
		Model   string `json:"model"`
		Version string `json:"version"`
	}
	if err = json.Unmarshal(payload, &announcement); err != nil {
		return err
	}
	if announcement.Schema != 1 {
		return errors.New("unsupported schema")
	}
	d := Device{Namespace: "account", Username: username, MAC: parts[3], Model: announcement.Model, Version: announcement.Version, Status: "pending"}
	if d.Model == "" || parts[3] != normalizeMAC(parts[3]) {
		return errors.New("invalid identity")
	}
	if err = validateDevice(&d); err != nil {
		return err
	}
	if d.Model == "tc1" && d.Version != "v2" {
		return errors.New("scoped TC1 requires v2")
	}
	a.mu.Lock()
	defer a.mu.Unlock()
	state, err := a.store.state(username)
	if err != nil || state != "active" {
		return errCredentials
	}
	var existing Device
	err = a.store.QueryRow("SELECT model,version,status FROM scoped_devices WHERE username=? AND mac=?", username, d.MAC).Scan(&existing.Model, &existing.Version, &existing.Status)
	if err == nil {
		if existing.Status == "hidden" || existing.Status == "deleting" {
			return nil
		}
		if existing.Model != d.Model || existing.Version != d.Version {
			return errConflict
		}
		if existing.Status == "active" {
			return nil
		}
	} else if !errors.Is(err, sql.ErrNoRows) {
		return err
	} else {
		_, err = a.store.Exec("INSERT INTO scoped_devices(username,mac,model,version,status) VALUES(?,?,?,?,'pending')", username, d.MAC, d.Model, d.Version)
		if err != nil {
			return err
		}
	}
	return a.syncScopedDevice(d)
}
func handleScopedDevice(w http.ResponseWriter, r *http.Request, mac string) {
	if !isValidMAC(mac) {
		jsonError(w, "MAC 地址格式不正确", 400)
		return
	}
	mac = normalizeMAC(mac)
	handleScopedDeviceFor(w, r, mac, currentUser(r))
}
func handleScopedDeviceFor(w http.ResponseWriter, r *http.Request, mac, u string) {
	app.mu.Lock()
	defer app.mu.Unlock()
	d := Device{Namespace: "account", Username: u, MAC: mac}
	err := app.store.QueryRow("SELECT model,version,status FROM scoped_devices WHERE username=? AND mac=?", u, mac).Scan(&d.Model, &d.Version, &d.Status)
	if err != nil {
		operationError(w, err)
		return
	}
	switch r.Method {
	case "DELETE":
		d.Status = "deleting"
		_, err = app.store.Exec("UPDATE scoped_devices SET status='deleting' WHERE username=? AND mac=?", u, mac)
	case "PUT":
		if d.Status != "hidden" {
			jsonError(w, "自动发现设备型号由固件声明，不能手动修改", 400)
			return
		}
		d.Status = "pending"
		_, err = app.store.Exec("UPDATE scoped_devices SET status='pending' WHERE username=? AND mac=?", u, mac)
	default:
		jsonError(w, "方法不允许", 405)
		return
	}
	if err == nil {
		err = app.syncScopedDevice(d)
	}
	if err != nil {
		operationError(w, err)
		return
	}
	jsonResp(w, map[string]string{"message": "设备已更新"})
}
