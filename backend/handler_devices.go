package main

import (
	"net/http"
	"strings"
)

func handleDevices(w http.ResponseWriter, r *http.Request) {
	u := currentUser(r)
	switch r.Method {
	case "GET":
		list, err := app.store.devices(u)
		if err != nil {
			operationError(w, err)
			return
		}
		for i := range list {
			list[i].Username = ""
		}
		jsonResp(w, list)
	case "POST":
		var d Device
		if err := decode(r, &d); err != nil {
			jsonError(w, err.Error(), 400)
			return
		}
		if d.Username != "" || d.Status != "" {
			jsonError(w, "不能指定设备归属或状态", 400)
			return
		}
		if err := validateDevice(&d); err != nil {
			jsonError(w, err.Error(), 400)
			return
		}
		if err := app.saveDevice(u, d, false); err != nil {
			operationError(w, err)
			return
		}
		jsonResp(w, map[string]string{"message": "设备已添加"})
	default:
		jsonError(w, "方法不允许", 405)
	}
}
func handleDevice(w http.ResponseWriter, r *http.Request) {
	mac := strings.TrimPrefix(r.URL.Path, "/api/devices/")
	if !isValidMAC(mac) {
		jsonError(w, "MAC 地址格式不正确", 400)
		return
	}
	mac = normalizeMAC(mac)
	switch r.Method {
	case "PUT":
		var d Device
		if err := decode(r, &d); err != nil {
			jsonError(w, err.Error(), 400)
			return
		}
		if d.MAC != "" && normalizeMAC(d.MAC) != mac || d.Username != "" || d.Status != "" {
			jsonError(w, "不能修改 MAC、归属或状态", 400)
			return
		}
		d.MAC = mac
		if err := validateDevice(&d); err != nil {
			jsonError(w, err.Error(), 400)
			return
		}
		if err := app.saveDevice(currentUser(r), d, true); err != nil {
			operationError(w, err)
			return
		}
		jsonResp(w, map[string]string{"message": "设备已更新"})
	case "DELETE":
		if err := app.removeDevice(currentUser(r), mac); err != nil {
			operationError(w, err)
			return
		}
		jsonResp(w, map[string]string{"message": "设备已解绑"})
	default:
		jsonError(w, "方法不允许", 405)
	}
}
