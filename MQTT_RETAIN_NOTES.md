# MQTT Retain Notes — Backend Reference

## Tổng quan

ESP32 subscribe các topic dưới đây với QoS 1 nhưng dùng **clean session** (mặc định).  
Nghĩa là message gửi lúc device offline **sẽ bị mất** trừ khi BE publish với `retain=true`.  
Khi device reconnect và subscribe lại, broker tự push retained message cuối — không cần code thêm ở ESP32.

---

## Các topic BE cần publish với `retain = true`

### 1. `inverter/{uid}/{deviceId}/cmd/settings`
- **Trigger**: server thay đổi setting của device
- **Payload**: `{}`
- **retain=true** để device nhận lại lệnh fetch setting ngay sau khi reconnect
- Sau khi device đã fetch xong, **không cần clear** vì payload `{}` là idempotent (fetch lại setting cũng không hại gì)

### 2. `inverter/{uid}/{deviceId}/cmd/schedule`
- **Trigger**: server thay đổi lịch của device
- **Payload**: `{}`
- **retain=true** — lý do tương tự cmd/settings

### 3. `inverter/{uid}/{deviceId}/share`
- **Trigger**: server push giá trị share (~mỗi 10s)
- **Payload**: `{"value": 1234}`
- **retain=true** để device nhận share value mới nhất ngay khi vừa reconnect, thay vì chờ lần push tiếp theo

### 4. `inverter/{uid}/{deviceId}/blacklist`
- **Trigger**: server khóa / mở khóa device (kill switch)
- **Payload**: `{"lock": true}` (khóa) hoặc `{"lock": false}` (mở khóa)
- **retain=true** — **quan trọng**: nếu device đang bị khóa mà reboot/reconnect, không có retained message thì device sẽ mất trạng thái khóa (clean session) và tự chạy lại. Retain đảm bảo device khóa lại ngay khi online.
- Khi khóa: ESP32 ghi `*LOCK12345#` xuống STM32 (ưu tiên tuyệt đối, đè cả share/schedule/setting), gửi lại định kỳ mỗi ~3s để STM32 vừa reboot cũng khóa lại.
- Khi mở khóa: ESP32 ghi một nhịp `*UNLOCK54321#` rồi trở lại giá trị bình thường (share > schedule > setting).

---

## OTA topic — KHÔNG dùng retain thông thường

### `inverter/{uid}/{deviceId}/firmware/update`
- **retain=true trực tiếp là nguy hiểm**: device reconnect → nhận lại message → trigger OTA lại, loop vô tận
- **Giải pháp đúng**: dùng pattern **publish → clear**
  1. BE publish trigger với `retain=true`
  2. Sau khi nhận được `ota/status` = `"success"` từ device → BE **clear retained message** bằng cách publish payload rỗng `""` với `retain=true` lên cùng topic
  3. Nếu device không báo success sau N phút → coi là failed, cũng clear để tránh device reboot loop

```
// Clear retained message (publish empty payload + retain=true)
mqttClient.publish("inverter/{uid}/{deviceId}/firmware/update", "", true);
```

### `inverter/{uid}/{deviceId}/ota/status`
- Topic này do **ESP32 publish** (không phải BE)
- BE subscribe để biết tiến trình OTA
- Không cần retain

---

## Tóm tắt nhanh

| Topic | Publisher | retain | Ghi chú |
|-------|-----------|--------|---------|
| `cmd/settings` | BE | ✅ true | idempotent, không cần clear |
| `cmd/schedule` | BE | ✅ true | idempotent, không cần clear |
| `share` | BE | ✅ true | device cần value mới nhất ngay khi online |
| `blacklist` | BE | ✅ true | giữ trạng thái khóa qua reboot/reconnect; payload `{"lock":bool}` |
| `firmware/update` | BE | ⚠️ true + phải clear sau khi device báo success | nguy hiểm nếu không clear |
| `ota/status` | ESP32 | ❌ false | realtime progress, không cần retain |
| `data` | ESP32 | ❌ false | realtime sensor data |
| `status` | ESP32 | ❌ false | dùng LWT thay thế (xem bên dưới) |

---

## Bonus: LWT (Last Will Testament) cho device status

Thay vì retain `status = online`, nên cấu hình LWT ở ESP32:
- **LWT topic**: `inverter/{uid}/{deviceId}/status`
- **LWT payload**: `{"status":"offline"}`
- **LWT retain**: `true`
- **LWT QoS**: 1

Khi device online, publish `{"status":"online", "updatedAt":"..."}` với `retain=false`.  
Khi device mất kết nối đột ngột, broker tự publish LWT `offline` với retain=true.  
→ Subscriber mới subscribe vào topic status sẽ thấy trạng thái thực tế của device.

> Hiện tại ESP32 chưa config LWT — cần thêm vào `connectToMqtt()`:
> ```cpp
> mqttClient.setWill(MQTT_TOPIC_STATUS.c_str(), "{\"status\":\"offline\"}", true, 1);
> ```
