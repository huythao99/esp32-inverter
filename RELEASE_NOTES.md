# Ghi chú phiên bản (hiển thị cho khách trên app / web)

Mỗi phiên bản là một mục `## <version>` (đúng bằng `currentFirmwareVersion`
trong `src/shared_state.cpp`), bên dưới mỗi dòng một thay đổi, viết cho khách
hàng đọc (tiếng Việt, ngắn gọn, không ghi chi tiết kỹ thuật nội bộ).

Workflow `.github/workflows/firmware.yml` đọc mục của phiên bản đang build và
gửi lên CMS cùng bản firmware. Sửa nội dung một mục đã phát hành rồi push lên
`main` thì CMS cũng được cập nhật theo. Phiên bản không có mục ở đây thì không
hiện gì cho khách (vẫn có thể viết trực tiếp trên CMS).

## 1.1.6
- Hỗ trợ bo mạch công suất đời đầu: thiết bị tự nhận ra loại bo và giao tiếp đúng cách
- Sửa lỗi thiết bị cũ không phát điện sau khi cập nhật firmware
- Thêm điều khiển bật/tắt hoà lưới từ xa
- Giảm sử dụng bộ nhớ, chạy ổn định hơn
