# SumatraPDF researcher v4.3 — đặc tả SDD

Ngày: 10/10/2026. Nền tảng: Windows x64, C++/Win32, mã nguồn hiện tại.

## Quyết định đã chốt

- AUTO bật khi mở PDF; tắt thủ công có hiệu lực trong tab đó, kể cả reload.
- 100% nội dung chính phải nhìn thấy. Lề trắng không bị cắt nếu vẫn còn chỗ.
- Vùng PDF thực tế sau khi trừ sidebar quyết định Single/Facing.
- Tiếng Việt; lời nghe diễn giải ý tưởng, không đọc ký hiệu toán.
- TTS chạy trên máy, tải bộ giọng một lần, chỉ nạp khi dùng.
- Phần diễn giải dùng nhà cung cấp AI hiện có. Offline TTS không đồng nghĩa offline AI.
- Connected Papers và Sci-Hub là dịch vụ ngoài; không lấy trạng thái dịch vụ giả làm thành công thật.
- Không commit, thay thế chương trình đang chạy, hay gửi tin nhắn Zalo tự động.

## Yêu cầu và bằng chứng

| Mã       | KHI … THÌ hệ thống PHẢI …                                                                                                                          | Kiểm chứng                                      |
| -------- | -------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------- |
| AUTO-01  | KHI mở PDF mới THÌ AUTO PHẢI bật, kể cả lịch sử cũ ghi OFF.                                                                                        | ad-hoc-auto-chat-layout                         |
| AUTO-02  | KHI chat làm hẹp vùng PDF THÌ Facing chỉ được giữ nếu hai vùng nội dung vừa tại zoom theo chiều cao.                                               | ad-hoc-auto-chat-layout                         |
| AUTO-03  | KHI resize, đóng chat hoặc bật lại AUTO THÌ bố cục PHẢI tính lại.                                                                                  | ad-hoc-auto-chat-layout                         |
| AUTO-04  | KHI người dùng tắt AUTO THÌ các mục view được mở lại, reload giữ OFF.                                                                              | ad-hoc-auto-chat-layout                         |
| AUTO-05  | KHI đọc nội dung THÌ không cắt chữ, bảng, công thức; hai trang có chiều cao khung bằng nhau, có gutter khi đủ chỗ.                                 | ad-hoc-reading-real-pdf, ad-hoc-facing-margins  |
| CHAT-01  | KHI chuyển giữa hai paper cùng thư mục THÌ không tự nhận phiên theo thư mục.                                                                       | ad-hoc-chat-paper-state                         |
| CHAT-02  | KHI quay lại paper THÌ lịch sử trong tab đó PHẢI còn.                                                                                              | ad-hoc-chat-paper-state                         |
| CHAT-03  | KHI Clear chat THÌ xóa transcript, bỏ session ID, dừng yêu cầu đang chạy; lượt sau là phiên mới.                                                   | ad-hoc-chat-paper-state                         |
| CHAT-04  | KHI phản hồi cũ đến sau Clear/đổi tài liệu THÌ không được nhập vào phiên mới.                                                                      | ad-hoc-chat-paper-state                         |
| AUDIO-01 | KHI tạo bản ý tưởng THÌ đọc text toàn bộ trang có đánh số; trang không có text phải được báo, không giả vờ đã đọc.                                 | research-tools: extraction                      |
| AUDIO-02 | KHI tạo lời nghe THÌ dùng tiếng Việt, giải thích bài toán, trực giác, phương pháp, kết quả, giả định và giới hạn; không dùng LaTeX hoặc công thức. | prompt contract + xem bản text trước TTS        |
| AUDIO-03 | KHI xuất MP3 THÌ bản text có thể xem/sửa; giọng tiếng Việt chạy local và có thể dùng lại sau khi ngắt mạng.                                        | real local TTS smoke test + MP3 probe           |
| AUDIO-04 | KHI chỉ đọc PDF bình thường THÌ không nạp model TTS; tác vụ dài không khóa UI.                                                                     | worker architecture + UI test                   |
| GRAPH-01 | KHI mở Connected Papers THÌ có cửa sổ web nhúng, query điền title/filename của paper và cho sửa thành DOI/arXiv.                                   | tạo cửa sổ đã thử; prefill website thật còn chờ |
| GRAPH-02 | KHI dịch vụ cần đăng nhập/hết quota THÌ hiển thị trạng thái thật của website.                                                                      | live smoke when available                       |
| SCI-01   | KHI tra Sci-Hub THÌ nhận DOI/PMID/URL, endpoint có thể cấu hình; CAPTCHA được xử lý bằng trình duyệt.                                              | URL/parser tests, embedded navigation           |
| SCI-02   | KHI tải PDF THÌ kiểm tra dữ liệu PDF, giới hạn dung lượng/thời gian, lưu theo đường dẫn người dùng chọn; không ghi đè ngầm.                        | local HTTP fixture                              |
| SCI-03   | KHI gọi mạng THÌ xác minh TLS; không kế thừa verify=False hoặc dò mirror bằng nguồn cũ của scihub.py.                                              | HTTP adapter contract                           |
| SHARE-01 | KHI Drive đăng nhập THÌ mở browser, scope drive.file, token bảo vệ DPAPI; upload theo chunk, hỗ trợ hủy.                                           | ad-hoc-document-share                           |
| SHARE-02 | KHI gửi Zalo THÌ chuẩn bị file và mở Zalo; người dùng chọn người nhận và gửi.                                                                      | ad-hoc-document-share                           |

## Kiến trúc

- Core đọc: DisplayModel, DocumentLayout. Core chat: trạng thái theo tab và mã yêu cầu độc lập.
- Research tools: metadata/text → dịch vụ diễn giải → text có thể xem → adapter TTS local → MP3.
- Dịch vụ ngoài: WebView2 nhúng Connected Papers; adapter Sci-Hub độc lập với engine PDF.
- Giữ C++ cho app; helper TTS/codec được tải và chạy riêng khi cần. Không đổi ngôn ngữ toàn chương trình.

## TDD và phát hành

1. Mỗi lỗi: test thể hiện hành vi sai trước, lưu log đỏ, sửa tối thiểu, lưu log xanh.
2. Mỗi tính năng: kiểm tra hợp đồng, đường lỗi và tác vụ thực tế phù hợp; mock không thay thế live test.
3. Review diff với từng mã yêu cầu, đặc biệt phản hồi chat muộn, quyền file, quoting và UI không khóa.
4. Chạy regression đọc/AUTO/share trên cùng binary release; đóng gói và kiểm SHA256.
5. Không ghi ĐẠT cho mục chưa có bằng chứng. Bản v4.2.1 vẫn là phương án quay về.

## Câu hỏi còn mở

- Chất lượng giọng Vietnamese local cần người dùng nghe thử; không coi model tồn tại là giọng đã đạt yêu cầu.
- OAuth Google Desktop client JSON chưa có: protocol đã thử bằng mock, tài khoản thật chưa thử.
- Dịch vụ Connected Papers/Sci-Hub có thể đổi domain, chặn nhúng, CAPTCHA/quota; không cam kết uptime.
- Cookie WebView không chuyển sang WinHTTP. Sau CAPTCHA, người dùng có thể cần tải trực tiếp trên website.
- Piper x_low 16 kHz là giọng khởi đầu nhỏ; MP3 hợp lệ chưa đủ chứng minh chất lượng phát âm được chấp nhận.
- Claude thật phải được kiểm lại trên paper dài; test parser và prompt dùng CLI giả lập.
