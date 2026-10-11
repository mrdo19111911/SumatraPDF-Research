# Checklist researcher v4.3

Đặc tả: [researcher-v43.md](spec/researcher-v43.md). Chỉ tick khi có bằng chứng.

## P0 — đọc và chat đúng tài liệu

- [x] AUTO bật mặc định khi mở PDF; kiểm fixture có trạng thái cũ OFF.
- [x] Tính Single/Facing theo diện tích PDF còn lại khi mở/đóng chat và resize.
- [x] Menu AUTO có check; chế độ thủ công bị làm xám; tắt AUTO và reload giữ OFF.
- [x] Regression 49 trang paper2: nội dung chính đầy đủ, footer thưa bị bỏ.
- [x] Chat riêng theo paper; bỏ tự nhận phiên gần nhất của thư mục.
- [x] Thêm Clear chat, reset session và dừng request.
- [x] Phản hồi cũ không quay lại sau Clear; đổi tab không lẫn nội dung.

## P1 — đọc paper bằng tai

- [x] Trích text toàn bộ paper, ghi số trang và trang thiếu text; fixture hai trang đã qua.
- [x] Luồng diễn giải tiếng Việt và prompt không công thức; parser Claude thật với CLI giả lập đã qua.
- [x] Mở bản text để xem/sửa; người dùng chọn bản đã duyệt trước TTS.
- [x] Tải bộ TTS tiếng Việt một lần; chạy local, lazy loading; tải thật đã qua.
- [x] Xuất MP3 thật qua native helper; codec MP3, thời lượng 13.344 giây; không ghi đè.
- [x] Hủy yêu cầu diễn giải đang chạy; UI phản hồi; không lưu bản bị hủy. Lỗi helper được trả về.
- [ ] Thử bản diễn giải từ Claude thật trên paper dài và kiểm đủ các phần.
- [ ] Người dùng nghe và chấp nhận chất lượng giọng nhỏ x_low.

## P1 — tài liệu liên quan và nguồn PDF

- [x] Cửa sổ WebView2 Connected Papers; đưa title/filename vào query; cho nhập DOI/arXiv. Test tạo cửa sổ đã qua.
- [x] Sci-Hub: DOI/PMID/URL, endpoint HTTPS có thể đổi, trình duyệt và nút Save PDF.
- [x] Kiểm định PDF bằng engine, timeout, giới hạn dung lượng, không ghi đè file có sẵn.
- [x] HTTP fixture: iframe/object, URL tương đối, CAPTCHA, PDF giả, HTTP lỗi và vượt dung lượng.
- [ ] Tạo graph Connected Papers thật; xác nhận prefill trên website hiện hành.
- [ ] Tra/tải Sci-Hub thật; CAPTCHA/cookies/endpoint phụ thuộc dịch vụ, chưa kiểm thành công.

## Chia sẻ đã làm, còn cấu hình thật

- [x] Drive: browser OAuth/PKCE, token DPAPI, upload chunk/hủy; test mock đã qua.
- [ ] Drive: đăng nhập/upload tài khoản thật — cần Desktop OAuth JSON.
- [x] Zalo: tìm ứng dụng, Browse nếu thiếu, chuẩn bị clipboard file; không tự gửi.

## Cổng phát hành

- [x] Review mã theo spec, sửa lỗi parser URL, đọc pipe nhanh và PowerShell 5.1.
- [x] Release build 0 lỗi/0 warning; regression AUTO/chat/share/toolbar/margins và 49 trang PDF đã qua.
- [x] README nêu tính năng/giới hạn; gói ZIP có manifest SHA256 và kiểm lại từng payload.
- [x] Smoke test portable không cần PDB: mở PDF, tìm script trong thư mục app, tạo MP3 thật; không thay bản cũ.

## Thanh công cụ v4.4

- [x] Icon trực tiếp: nền tối, A+/A−, chat/Clear, text, diễn giải Việt, MP3, Connected Papers, Sci-Hub, Drive, Zalo.
- [x] Nhấn icon nền tối thật; text/diễn giải/MP3 đi thẳng tới hộp chọn file.
- [x] EPUB đổi font độc lập với zoom; PDF không giả lập đổi font bằng zoom.
- [x] Kiểm nút không bị khuất ở cửa sổ rộng/hẹp; giữ toolbar top/right và AUTO theo vùng PDF.
- [ ] Reading mode đổi font PDF, bảo toàn nội dung và thứ tự đọc.
- [ ] Font Markdown/HTML web.
- [x] AI có trích dẫn trang gốc, glossary, bước chứng minh và so sánh paper trong v4.5.

## Trợ lý nghiên cứu v4.5

- [x] Bảy icon riêng: nguồn, đoạn chọn, ký hiệu, so sánh paper, phiên bản, hình/bảng và Reading view.
- [x] Nguồn của từng PDF có ID và số trang vật lý; lấy toàn bộ trang, báo trang thiếu text.
- [x] Bấm nguồn mở đúng file/trang, tìm quote trên đúng trang và đánh dấu; vùng hình có tọa độ giới hạn trong page.
- [x] So sánh nhận nhiều PDF; phiên bản nhận bản cũ rồi bản mới.
- [x] Hình/bảng nhận PNG thật từ vùng chọn hoặc trang hiện tại.
- [x] Reading view có A+/A− riêng, text trích xuất đủ trang và ảnh gốc tải khi mở.
- [x] Worker không chặn UI; Clear hủy provider và loại kết quả cũ. Báo cáo lưu Markdown.
- [x] TDD: `v45-red.log`, `v45-assistant3.log` (highlight mặc định tắt), `v45-assistant4.log` xanh.
- [ ] Xác nhận chất lượng suy luận, glossary và phục dựng thứ tự đọc với Claude thật trên paper phức tạp.
- [ ] Reading mode phục dựng công thức/vector và layout PDF hoàn toàn không mất dữ liệu.

Spec: `docs/spec/research-assistant-v45.md`. Nguồn bấm được trong report/chat; link trong Markdown xuất ra không phải chuẩn di động cho app khác.

Kiểm tra v4.5: `v45-build7.log` (0 lỗi/0 warning), `v45-final-ad-hoc-research-assistant.log`,
`v45-final-toolbar2.log`, `v45-final-ad-hoc-adaptive-toolbar.log`. Header report có test pixel
đỏ `v45-header-red.log` rồi sửa paint và kiểm xanh. `v45-real-source.log` kiểm đủ 49 trang
paper2 thật (121.513 byte UTF-8), không gọi AI.

Fixture AUTO/right dùng cửa sổ 1900px thay 1800px để vùng PDF vẫn ngang khi toolbar có
thêm icon; giữ kiểm tra Single khi hai trang không vừa và Facing khi tăng chiều rộng.

Regression cuối: `v45-verified-ad-hoc-auto-chat-layout.log`, `v45-verified-ad-hoc-chat-paper-state.log`,
`v45-verified-ad-hoc-research-tools.log`. Kiểm UI/engine/HTTP nội bộ; không chứng nhận suy luận Claude thật.

Spec: `docs/spec/toolbar-v44.md`. Đỏ: `toolbar-icons-before.log`. Xanh: `toolbar-icons-test.log`. Build: `toolbar-icons-build.log`.

Regression v4.4: `v44-ad-hoc-*.log`, `v44-research.log`, `v44-reading-real.log`. Kiểm gutter dựa trên lề PDF và scale thật; không dùng ngưỡng pixel cố định khi toolbar xuống dòng làm đổi chiều cao viewport.

## Review v4.6

- [x] Reading/report theo Dark mode ngay khi đổi màu; giữ cỡ chữ và nội dung.
- [x] KaTeX cục bộ cho bốn dạng dấu toán học; giữ code nguyên văn.
- [x] Hủy tác vụ khi đóng/thay paper; kiểm lại file sau dialog.
- [x] Giữ ID nguồn ổn định khi Clear một provider.
- [x] Report tách khỏi phiên chat chưa nhận nội dung; tiêu đề có tên PDF và tác vụ.
- [ ] Chứng nhận suy luận toán học, OCR, thứ tự đọc và công thức trên corpus thật.

Spec: `docs/spec/research-assistant-v46.md`. Review: `docs/REVIEW-v4.6.md`.
TDD đỏ: `v46-red.log`, `v46-lifetime-red2.log`, `v46-sources-red.log`, `v46-history-red.log`.

## Log TDD v4.3

- AUTO đỏ: `.work/toolchain/auto-chat-before.log`, `auto-default-before.log`.
- AUTO xanh: `tests/ad-hoc-auto-chat-layout.ts`, bao gồm bật/tắt và reload. Menu test cần menubar hiện và đợi WM_COMMAND được xử lý; giữ nguyên kiểm tra trạng thái.
- Clear chat đỏ: `CmdClearAIChat` chưa tồn tại trên binary cũ, `ad-hoc-chat-paper-state` thất bại ngày 10/10.
- Clear chat xanh: `.work/toolchain/v43-ad-hoc-chat-paper-state.log`.
- Research đỏ: `research-before.log`, `research-tools-after.log`, `research-tools-after2.log`.
- TTS đỏ: `research-async-before.log` (PowerShell 5.1 thiếu StandardInputEncoding).
- Hủy worker đỏ: `research-async-before2.log` (chưa có test seam); thêm seam gọi luồng production.
- Research/TTS/hủy xanh: `.work/toolchain/research-final.log`; Claude và HTTP giả lập, Piper/FFmpeg thật.
- Regression cuối: `.work/toolchain/v43-*.log`, cùng binary build `research-tools-build8.log`.
- Portable: `.work/toolchain/v43-portable-smoke.log`. ZIP: `.work/toolchain/package-v43.log`.

## Thư viện paper v4.7

- [x] Hai icon arXiv search và Collect referenced papers.
- [x] Trích bibliography, cho sửa danh sách, báo nguồn và lưu JSON; không ghi đè.
- [x] Worker tuần tự arXiv → Sci-Hub → Anna's Archive; Stop/đóng giữ file đã tải.
- [x] Thay nhánh LibGen bằng website Anna hoặc API CrazyZard tùy cấu hình.
- [x] Kiểm cuối hidden fixtures, toolbar, browser và ZIP portable.
- [ ] Tải sách Atiyah–Macdonald thật qua Anna: chưa thành công; native TLS lỗi, browser yêu cầu xác minh.

Spec: `docs/spec/paper-library-v47.md`. Không tính fixture nội bộ là tải sách thật.
