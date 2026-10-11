# Thanh công cụ v4.4

Mục tiêu: thao tác thường dùng có icon trực tiếp, tooltip rõ, không bị khuất khi đổi kích thước cửa sổ. AUTO giữ chữ đậm và trạng thái bật/tắt.

| Yêu cầu | Hành vi                                                                                                             | Kiểm thử                                                      |
| ------- | ------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------- |
| BAR-01  | Icon nền tối đổi giao diện và màu tài liệu theo chế độ smart; nhấn lại trở về sáng.                                 | Nhấn icon thật, kiểm trạng thái theme và smart.               |
| BAR-02  | A+/A− đổi cỡ chữ và dàn lại EPUB; giữ zoom trang. PDF, Markdown/HTML web và tài liệu không dàn lại được có nút xám. | EPUB 12→14→12; PDF disabled; zoom giữ nguyên.                 |
| BAR-03  | Chat, Clear, text toàn paper, diễn giải Việt, MP3, Connected Papers, Sci-Hub, Drive và Zalo có icon.                | Kiểm icon, visibility, enabled.                               |
| BAR-04  | Text, diễn giải và MP3 mở thẳng hộp chọn file, không qua menu Research.                                             | Nhấn từng icon thật; kiểm tiêu đề hộp chọn file; Cancel.      |
| BAR-05  | Thanh công cụ xuống dòng hoặc sang nhiều cột; giữ tất cả nút trong vùng hiển thị.                                   | Cửa sổ 1400×850, 500×900; top/right và annotation regression. |
| BAR-06  | Cấu hình giọng/FFmpeg vẫn ở icon bình thí nghiệm; mạng và AI chỉ chạy khi chọn tác vụ.                              | Research worker/TTS regression.                               |

UI gọi command; command gọi dịch vụ ResearchTools/DocumentShare hoặc ebook settings. HTTP và TTS giữ adapter riêng. SVG dùng hệ icon hiện có, không thêm thư viện UI.

## Lệnh mới

- `CmdToggleReadingDarkMode`: bật/tắt nền tối đọc tài liệu, dùng DocumentColorsFollowTheme=smart.
- `CmdIncreaseTextSize`: tăng font tài liệu dàn lại được thêm 2 pt, giới hạn 8–72 pt.
- `CmdDecreaseTextSize`: giảm font tương ứng.
- `CmdResearchNarration`: tạo bản diễn giải tiếng Việt.
- `CmdResearchExportText`: xuất text mọi trang PDF.
- `CmdResearchExportMp3`: xuất MP3 từ text đã duyệt.

Checkout website `../hack/webapps/sumatra-website/www/docs` không có trên máy này; danh sách lệnh đang được ghi trong tài liệu custom này.

## Chưa triển khai

- Reading mode cho PDF với cỡ chữ độc lập, giữ thứ tự đọc và công thức/hình/bảng.
- Cỡ chữ cho Markdown/HTML trong WebView.
- AI trích dẫn bấm về trang gốc, glossary ký hiệu, giải thích từng bước chứng minh và so sánh nhiều paper.

Các giới hạn dịch vụ thật và chất lượng giọng đọc trong spec v4.3 vẫn còn. Icon không loại bỏ bước cấu hình ban đầu hoặc bước chọn file/người nhận.
