/* ===========================================================================
 * gti_fota.h - phia ESP32 cua viec nap firmware cho STM32 qua UART
 *
 * Dung chung cho ca bo hoa luoi dung STM32F303 va STM32G431: giao thuc y het
 * nhau, chi khac kich thuoc vung ung dung ma bootloader tu bao trong HELLO.
 *
 * ---------------------------------------------------------------------------
 * CACH DUNG
 *
 *     #include "gti_fota.h"
 *     GtiFota fota(Serial2);          // dung cai UART dang noi voi STM32
 *
 *     // anh firmware da tai ve va luu trong LittleFS
 *     File f = LittleFS.open("/app.bin", "r");
 *     GtiFotaResult r = fota.update(
 *         f.size(), crc32_tu_server,
 *         [&f](uint32_t off, uint8_t *buf, uint32_t n) -> bool {
 *             return f.seek(off) && f.read(buf, n) == (int)n;
 *         });
 *
 * update() tu lam het: bao STM32 dung may, doi no reset sang bootloader, xoa
 * flash, day tung manh, kiem CRC, roi cho chay firmware moi. Ham chay dong bo
 * va mat khoang 35 giay o 9600 baud. May dang ngoi san trong bootloader (sau
 * rescue() hay mot lan nap bo do) thi update() cung nap tiep duoc.
 *
 * ---------------------------------------------------------------------------
 * PHIEN BAN DIEN AP (12/24/36/48V) - tu bootloader v2
 *
 * Bon ban firmware F303 chi khac tham so, nap nham ban 24V vao may 48V thi
 * CRC van dung ma may chay sai. Nen:
 *  - board mang ma phien ban trong option byte Data0 (0x12/0x24/0x36/0x48,
 *    ghi luc san xuat bang flash-rdp.ps1), HELLO bao ra o byte 23;
 *  - anh app.bin mang ma cua no trong 16 byte cuoi ("GTIV" + ma, do
 *    make-fota-image.ps1 them vao).
 * update() so hai ma TRUOC khi xoa flash. Lech thi tra GTI_FOTA_ERR_VARIANT va
 * cho may chay lai firmware cu. Bootloader v2 cung tu kiem lai lan nua.
 *
 * Board F303 con bootloader v1 (khong bao ma): update() tu choi, tru khi da
 * goi assumeVariant(ma) - ESP32 tu chiu trach nhiem biet board la ban nao.
 * G431 chua co co che nay: khong kiem, nhu truoc.
 *
 * ---------------------------------------------------------------------------
 * NHUNG DIEU PHAI GIU DUNG
 *
 *  - GIU LAI ANH CU trong LittleFS. Day la duong quay ve duy nhat khi ban moi
 *    chay sai: goi update() lan nua voi file cu la xong. STM32 khong con cho
 *    de tu giu hai ban.
 *  - KHONG goi update() khi bo hoa luoi dang phat cong suat lon. STM32 tu dung
 *    may truoc khi reset, nhung cat tai dot ngot van khong hay.
 *  - Trong luc update() chay thi dung gui bat ky thu gi khac vao UART do.
 *  - Toc do UART giu nguyen 9600 tu dau den cuoi, khong doi baud.
 * =========================================================================== */

#ifndef GTI_FOTA_H
#define GTI_FOTA_H

#include <Arduino.h>
#include <functional>

/* --- phai khop fota_proto.h ben STM32 --- */
#define GTI_FOTA_SOF            0x7E
#define GTI_FOTA_RESP_BIT       0x80
#define GTI_FOTA_CHUNK          256      /* boi so cua 8 */

#define GTI_CMD_HELLO           0x01
#define GTI_CMD_BEGIN           0x02
#define GTI_CMD_DATA            0x03
#define GTI_CMD_END             0x04
#define GTI_CMD_GO              0x05
#define GTI_CMD_PING            0x06

#define GTI_ST_VARIANT_ERR      0x09     /* v2: sai / chua gan phien ban dien ap */

#define GTI_FAMILY_F303         0
#define GTI_FAMILY_G431         1

/* Ma phien ban dien ap (fota_proto.h, muc PHIEN BAN DIEN AP) */
#define GTI_VARIANT_NONE        0x00     /* dong chip khong kiem (G431) */
#define GTI_VARIANT_UNSET       0xFF     /* board chua gan Data0 */
#define GTI_VARIANT_UNREPORTED  0xFE     /* chi ben ESP32: bootloader v1 khong bao */

/* Loai san pham - option byte Data1 tren F303 (bootloader v3). Cung mot muc
 * dien ap co the la bo hoa luoi, bo sac hay bo hybrid; ba thu do dung mach
 * cong suat khac han nhau nen phai phan biet. */
#define GTI_PRODUCT_GTI         0x01     /* bo hoa luoi */
#define GTI_PRODUCT_CHARGER     0x02     /* bo sac */
#define GTI_PRODUCT_HYBRID      0x03     /* bo hybrid */
#define GTI_PRODUCT_UNSET       0xFF     /* board/anh chua khai bao loai */
#define GTI_PRODUCT_NONE        0x00     /* dong chip khong kiem (G431) */
#define GTI_FOTA_TAIL_MAGIC     0x56495447u   /* "GTIV" */
#define GTI_FOTA_TAIL_SIZE      16

enum GtiFotaResult
{
	GTI_FOTA_OK = 0,
	GTI_FOTA_ERR_NO_ACK,        /* ung dung khong tra loi "*FOTAOK#" */
	GTI_FOTA_ERR_BUSY,          /* may dang ban ghi flash, thu lai sau */
	GTI_FOTA_ERR_NO_BOOTLOADER, /* khong bat duoc bootloader sau khi reset */
	GTI_FOTA_ERR_BEGIN,
	GTI_FOTA_ERR_DATA,
	GTI_FOTA_ERR_END_CRC,       /* nap xong nhung CRC32 khong khop */
	GTI_FOTA_ERR_READ,          /* doc file anh that bai */
	GTI_FOTA_ERR_VARIANT,       /* anh khac phien ban dien ap cua board, hoac
	                             * board/anh khong mang ma - xem boardVariant(),
	                             * imageVariant() */
	GTI_FOTA_ERR_PRODUCT        /* anh cua LOAI SAN PHAM khac (vd firmware hoa
	                             * luoi cho bo sac) - xem boardProduct(),
	                             * imageProduct() */
};

class GtiFota
{
public:
	/* readAt: doc n byte tai offset off trong anh firmware, tra ve false neu
	 * doc khong duoc. */
	typedef std::function<bool(uint32_t off, uint8_t *buf, uint32_t n)> ReadFn;

	explicit GtiFota(Stream &port) : _p(port) { }

	/* Ghi log tien trinh ra day, vd Serial. De nullptr thi im lang. */
	void setLog(Print *log) { _log = log; }

	/* Chi cho board F303 con bootloader v1 (HELLO khong co ma phien ban): ma
	 * ESP32 tu biet ve board nay (vd 0x48), update() dung no thay cho board.
	 * Mac dinh 0xFF = khong biet -> tu choi nap. */
	void assumeVariant(uint8_t code) { _assumed = code; }

	GtiFotaResult update(uint32_t size, uint32_t crc32, ReadFn readAt)
	{
		uint8_t  pay[8];
		uint8_t  chunk[4 + GTI_FOTA_CHUNK];
		uint32_t off;

		/* --- 0. doc ma phien ban trong duoi anh, chua dong gi den STM32 --- */
		if (!readTail(size, readAt)) { return GTI_FOTA_ERR_READ; }

		/* --- 1. bao ung dung dung may va sang bootloader --- */
		flush();
		_p.print("*FOTA12345#");
		{
			/* Ung dung van dang ban telemetry moi giay mot lan, nen "*FOTAOK#"
			 * hoan toan co the den SAU mot khung so lieu. Phai doc tiep cho
			 * den khi thay dung chu can tim chu khong duoc dung o khung dau. */
			const int8_t a = waitAck(3000);
			if (a == 1) { return GTI_FOTA_ERR_BUSY; }
			if (a < 0)
			{
				/* Khong co ung dung tra loi - co the may DANG o san trong
				 * bootloader (sau rescue(), hay lan nap truoc bo do / bi tu
				 * choi o END). Hoi thang bootloader truoc khi bao loi. */
				if (!helloRetry(2)) { return GTI_FOTA_ERR_NO_ACK; }
				say("may dang o san trong bootloader");
			}
			else
			{
				say("ung dung da nhan lenh, dang dung may...");

				/* Ung dung con phai luu so dien va cho dong dien ve 0 truoc
				 * khi reset. */
				delay(1500);

				/* --- 2. bat tay voi bootloader --- */
				if (!helloRetry(20)) { return GTI_FOTA_ERR_NO_BOOTLOADER; }
				say("da vao bootloader");
			}
		}

		/* --- 2b. dung phien ban dien ap chua - flash van con nguyen --- */
		if (!variantOk())
		{
			leaveToApp();
			return GTI_FOTA_ERR_VARIANT;
		}

		/* --- 2c. dung loai san pham chua (hoa luoi / sac / hybrid) --- */
		if (!productOk())
		{
			leaveToApp();
			return GTI_FOTA_ERR_PRODUCT;
		}

		/* --- 3. xoa vung ung dung --- */
		put32(&pay[0], size);
		put32(&pay[4], crc32);
		if (!command(GTI_CMD_BEGIN, pay, 8, 8000))
		{
			if (_status == GTI_ST_VARIANT_ERR)
			{
				/* bootloader tu choi TRUOC khi xoa: board chua gan Data0 */
				say("bootloader: board chua gan phien ban (Data0)");
				leaveToApp();
				return GTI_FOTA_ERR_VARIANT;
			}
			return GTI_FOTA_ERR_BEGIN;
		}
		say("da xoa flash");

		/* --- 4. day tung manh --- */
		for (off = 0; off < size; off += GTI_FOTA_CHUNK)
		{
			uint32_t n = size - off;
			uint32_t padded;

			if (n > GTI_FOTA_CHUNK) { n = GTI_FOTA_CHUNK; }

			/* Manh cuoi don len boi so cua 8 bang 0xFF: ca hai dong chip deu
			 * ghi flash theo khoi 8 byte. 0xFF la gia tri cua flash vua xoa
			 * nen khong anh huong gi den CRC32 (CRC chi tinh tren size that). */
			padded = (n + 7u) & ~7u;
			memset(&chunk[4], 0xFF, padded);
			if (!readAt(off, &chunk[4], n)) { return GTI_FOTA_ERR_READ; }

			put32(&chunk[0], off);

			if (!commandRetry(GTI_CMD_DATA, chunk, 4 + padded, 2000, 3))
			{
				return GTI_FOTA_ERR_DATA;
			}
			if ((off % 4096u) == 0u) { say("  %u / %u byte", off, size); }
		}

		/* --- 5. chot: bootloader tu tinh CRC32 toan bo va ghi nhan --- */
		if (!command(GTI_CMD_END, nullptr, 0, 5000))
		{
			/* Sai phien ban o day nghia la buoc 2b bi bo qua (vd assumeVariant
			 * sai). Flash da xoa: may o lai bootloader, goi update() voi anh
			 * dung la nap tiep duoc. */
			if (_status == GTI_ST_VARIANT_ERR)
			{
				say("bootloader tu choi anh: sai phien ban dien ap");
				return GTI_FOTA_ERR_VARIANT;
			}
			return GTI_FOTA_ERR_END_CRC;
		}
		say("CRC32 khop, firmware moi da duoc chap nhan");

		/* --- 6. chay --- */
		command(GTI_CMD_GO, nullptr, 0, 2000);
		return GTI_FOTA_OK;
	}

	/* Duong cuu ho: firmware trong may hong toi muc khong tra loi "*FOTA12345#"
	 * nua. Goi ham nay roi bao nguoi o hien truong TAT BAT NGUON bo hoa luoi.
	 * Bootloader nghe 400ms sau moi lan reset, bat duoc la no o lai cho nap.
	 * Tra ve true khi da bat duoc bootloader. */
	bool rescue(uint32_t timeout_ms = 60000)
	{
		const uint32_t t0 = millis();

		say("dang goi bootloader - hay tat bat nguon bo hoa luoi");
		while ((millis() - t0) < timeout_ms)
		{
			_p.print("*BOOTME#");
			delay(40);
			if (hello(200)) { say("da bat duoc bootloader"); return true; }
		}
		return false;
	}

	/* Thong tin doc duoc tu lenh HELLO gan nhat. */
	uint8_t  family()   const { return _family; }      /* 0 = F303, 1 = G431 */
	uint8_t  blVersion() const { return _blVer; }
	uint32_t appMax()   const { return _appMax; }
	uint32_t appSize()  const { return _appSize; }
	/* Ma phien ban cua board: 0x12..0x48, GTI_VARIANT_UNSET (chua gan Data0),
	 * GTI_VARIANT_NONE (G431) hoac GTI_VARIANT_UNREPORTED (bootloader v1). */
	uint8_t  boardVariant() const { return _variant; }

	/* Ma loai san pham cua board: GTI_PRODUCT_GTI/CHARGER/HYBRID,
	 * GTI_PRODUCT_UNSET (chua gan Data1) hoac GTI_PRODUCT_NONE (G431 / bootloader
	 * cu hon v3). Va ma loai trong duoi cua anh vua dua vao update(). */
	uint8_t  boardProduct() const { return _product; }
	uint8_t  imageProduct() const { return _imgProduct; }

	/* Ma phien ban trong duoi cua anh vua dua vao update(); GTI_VARIANT_NONE
	 * neu anh khong co duoi (anh G431, hay F303 build bang script cu). */
	uint8_t  imageVariant() const { return _imgVariant; }

	/* Phien ban firmware ghi trong duoi anh (GTI_FW_VERSION luc build), dang
	 * 0x00MMmmpp - vd 0x00020000 = 2.0.0. 0xFFFFFFFF = file khong ghi. Doc
	 * duoc ngay sau update(), ke ca khi update() tu choi. Day la phien ban
	 * cua FILE; board dang chay ban nao thi xem truong 13 cua telemetry
	 * (chuoi "M.m.p", tu firmware 2.0.0 tro di). */
	uint32_t imageVersion() const { return _imgVersion; }

private:
	Stream  &_p;
	Print   *_log    = nullptr;
	uint8_t  _family = 0xFF;
	uint8_t  _blVer  = 0;
	uint8_t  _variant = GTI_VARIANT_UNREPORTED;
	uint8_t  _assumed = GTI_VARIANT_UNSET;
	uint8_t  _imgVariant = GTI_VARIANT_NONE;
	uint8_t  _product = GTI_PRODUCT_NONE;
	uint8_t  _imgProduct = GTI_PRODUCT_UNSET;
	uint8_t  _imgFamily  = 0xFF;
	uint32_t _imgVersion = 0xFFFFFFFFu;
	uint8_t  _status = 0xFF;                    /* ma trang thai khung tra loi gan nhat */
	uint32_t _appMax = 0;
	uint32_t _appSize = 0;
	uint8_t  _rx[288];
	uint16_t _rxLen = 0;

	static void put32(uint8_t *p, uint32_t v)
	{
		p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
		p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
	}
	static uint32_t get32(const uint8_t *p)
	{
		return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
		     | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
	}

	static uint16_t crc16(const uint8_t *p, uint32_t n, uint16_t crc)
	{
		while (n--)
		{
			crc ^= (uint16_t)(*p++) << 8;
			for (uint8_t i = 0; i < 8; i++)
			{
				crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
			}
		}
		return crc;
	}

	void say(const char *fmt, ...)
	{
		if (!_log) { return; }
		char buf[96];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(buf, sizeof(buf), fmt, ap);
		va_end(ap);
		_log->print("[fota] "); _log->println(buf);
	}

	void flush() { while (_p.available()) { _p.read(); } }

	/* Duoi 16 byte fota_tail_t: "GTIV", ma, family, 0xFFFF, fw_version (0x00MMmmpp),
	 * chk = ~(w0 + w1 + w2). Khong co duoi hop le -> _imgVariant = NONE. Chi
	 * tra false khi doc file loi. */
	bool readTail(uint32_t size, ReadFn &readAt)
	{
		uint8_t t[GTI_FOTA_TAIL_SIZE];

		_imgVariant = GTI_VARIANT_NONE;
		_imgFamily  = 0xFF;
		_imgProduct = GTI_PRODUCT_UNSET;
		_imgVersion = 0xFFFFFFFFu;
		if (size < GTI_FOTA_TAIL_SIZE) { return true; }
		if (!readAt(size - GTI_FOTA_TAIL_SIZE, t, GTI_FOTA_TAIL_SIZE)) { return false; }

		const uint32_t w0 = get32(&t[0]), w1 = get32(&t[4]), w2 = get32(&t[8]);
		if ((w0 == GTI_FOTA_TAIL_MAGIC) && (get32(&t[12]) == ~(w0 + w1 + w2)))
		{
			_imgVariant = t[4];
			_imgFamily  = t[5];
			/* v3: anh sinh truoc bootloader v3 co 0xFF o day (truong reserved cu),
			 * doc ra dung thanh "chua khai bao" nen khong can sinh lai anh cu. */
			_imgProduct = t[6];
			_imgVersion = w2;
			if (w2 != 0xFFFFFFFFu)
			{
				say("file: phien ban dien ap %02X, firmware %u.%u.%u", _imgVariant,
				    (unsigned)((w2 >> 16) & 0xFFu), (unsigned)((w2 >> 8) & 0xFFu), (unsigned)(w2 & 0xFFu));
			}
		}
		return true;
	}

	/* So ma cua anh voi board, sau HELLO, truoc BEGIN. */
	bool variantOk()
	{
		uint8_t board = _variant;

		if ((board == GTI_VARIANT_UNREPORTED) && (_family == GTI_FAMILY_F303))
		{
			if (_assumed == GTI_VARIANT_UNSET)
			{
				say("bootloader v1 khong bao phien ban dien ap - tu choi");
				say("(nap bootloader v2, hoac goi assumeVariant())");
				return false;
			}
			board = _assumed;
		}

		if ((board == GTI_VARIANT_NONE) || (board == GTI_VARIANT_UNREPORTED))
		{
			/* dong chip khong kiem; anh co duoi thi it nhat dung dong chip */
			if ((_imgVariant != GTI_VARIANT_NONE) && (_imgFamily != _family))
			{
				say("anh cua dong chip khac (anh %u, board %u)", _imgFamily, _family);
				return false;
			}
			return true;
		}

		if (board == GTI_VARIANT_UNSET)
		{
			say("board chua gan phien ban (Data0) - nap lai bang flash-rdp.ps1");
			return false;
		}
		if (_imgVariant == GTI_VARIANT_NONE)
		{
			say("anh khong co ma phien ban - tao lai bang make-fota-image.ps1 moi");
			return false;
		}
		if ((_imgVariant != board) || (_imgFamily != _family))
		{
			say("SAI PHIEN BAN: anh %02X, board %02X - khong nap", _imgVariant, board);
			return false;
		}
		say("phien ban khop: %02X", board);
		return true;
	}

	/* So LOAI SAN PHAM cua anh voi board. Chi kiem khi ca hai ben deu khai bao:
	 * board nap truoc bootloader v3 chua gan Data1, va anh sinh truoc v3 khong
	 * mang ma loai. Bo qua hai truong hop do thi tinh nang moi chi them bao ve
	 * chu khong chan duong nap dang chay. */
	bool productOk()
	{
		if ((_product == GTI_PRODUCT_NONE) || (_product == GTI_PRODUCT_UNSET))
		{
			return true;
		}
		if (_imgProduct == GTI_PRODUCT_UNSET)
		{
			say("anh khong co ma loai san pham - bo qua buoc kiem");
			return true;
		}
		if (_imgProduct != _product)
		{
			say("SAI LOAI SAN PHAM: anh %02X, board %02X - khong nap",
			    _imgProduct, _product);
			return false;
		}
		say("loai san pham khop: %02X", _product);
		return true;
	}

	/* Tu choi khi flash chua bi xoa: cho may chay lai firmware cu ngay, khoi
	 * doi 60 giay. GO bi tu choi (khong co ung dung) thi may o lai bootloader. */
	void leaveToApp()
	{
		if (command(GTI_CMD_GO, nullptr, 0, 2000)) { say("da cho chay lai firmware cu"); }
	}

	/* Tra ve 0 = "*FOTAOK#", 1 = "*FOTABUSY#", -1 = het gio.
	 * Bo qua moi khung telemetry xen vao giua. */
	int8_t waitAck(uint32_t timeout_ms)
	{
		const uint32_t t0 = millis();
		char tail[12] = { 0 };

		while ((millis() - t0) < timeout_ms)
		{
			while (_p.available())
			{
				memmove(tail, tail + 1, sizeof(tail) - 2);
				tail[sizeof(tail) - 2] = (char)_p.read();
				if (strstr(tail, "*FOTAOK#"))   { return 0; }
				if (strstr(tail, "*FOTABUSY#")) { return 1; }
			}
			delay(1);
		}
		return -1;
	}

	void sendFrame(uint8_t cmd, const uint8_t *pay, uint16_t len)
	{
		uint8_t head[4] = { GTI_FOTA_SOF, cmd, (uint8_t)len, (uint8_t)(len >> 8) };
		uint16_t c = crc16(&head[1], 3, 0xFFFF);

		if (len) { c = crc16(pay, len, c); }

		_p.write(head, 4);
		if (len) { _p.write(pay, len); }
		_p.write((uint8_t)(c & 0xFF));
		_p.write((uint8_t)(c >> 8));
	}

	/* Doc khung tra loi. true = khung hop le va ma trang thai = 0 (OK).
	 * Ma trang thai cua khung hop le nam lai trong _status. */
	bool recvFrame(uint8_t cmd, uint32_t timeout_ms)
	{
		const uint32_t t0 = millis();
		uint8_t head[3];
		uint16_t n, got = 0, c;

		_status = 0xFF;

		/* cho byte mo dau */
		for (;;)
		{
			if ((millis() - t0) >= timeout_ms) { return false; }
			if (!_p.available()) { delay(1); continue; }
			if ((uint8_t)_p.read() == GTI_FOTA_SOF) { break; }
		}

		if (!readN(head, 3, t0, timeout_ms)) { return false; }
		if (head[0] != (uint8_t)(cmd | GTI_FOTA_RESP_BIT)) { return false; }

		n = (uint16_t)head[1] | ((uint16_t)head[2] << 8);
		if (n > sizeof(_rx)) { return false; }
		if (!readN(_rx, n, t0, timeout_ms)) { return false; }

		uint8_t tail[2];
		if (!readN(tail, 2, t0, timeout_ms)) { return false; }
		got = (uint16_t)tail[0] | ((uint16_t)tail[1] << 8);

		c = crc16(head, 3, 0xFFFF);
		c = crc16(_rx, n, c);
		if (c != got) { return false; }

		_rxLen = n;
		if (n >= 1) { _status = _rx[0]; }
		return (n >= 1) && (_rx[0] == 0);
	}

	bool readN(uint8_t *dst, uint16_t n, uint32_t t0, uint32_t timeout_ms)
	{
		uint16_t i = 0;

		while (i < n)
		{
			if ((millis() - t0) >= timeout_ms) { return false; }
			if (_p.available()) { dst[i++] = (uint8_t)_p.read(); }
			else { delay(1); }
		}
		return true;
	}

	bool command(uint8_t cmd, const uint8_t *pay, uint16_t len, uint32_t timeout_ms)
	{
		flush();
		sendFrame(cmd, pay, len);
		return recvFrame(cmd, timeout_ms);
	}

	bool commandRetry(uint8_t cmd, const uint8_t *pay, uint16_t len,
	                  uint32_t timeout_ms, uint8_t tries)
	{
		while (tries--)
		{
			if (command(cmd, pay, len, timeout_ms)) { return true; }
		}
		return false;
	}

	bool hello(uint32_t timeout_ms)
	{
		if (!command(GTI_CMD_HELLO, nullptr, 0, timeout_ms)) { return false; }
		if (_rxLen < 23) { return false; }
		_family  = _rx[1];
		_blVer   = _rx[2];
		_appMax  = get32(&_rx[9]);
		_appSize = get32(&_rx[17]);
		/* v2 them byte 23; v1 chi co 23 byte */
		_variant = (_rxLen >= 24) ? _rx[23] : GTI_VARIANT_UNREPORTED;
		/* v3 them byte 24. Bootloader cu khong bao -> coi nhu khong kiem loai. */
		_product = (_rxLen >= 25) ? _rx[24] : GTI_PRODUCT_NONE;
		return true;
	}

	bool helloRetry(uint8_t tries)
	{
		while (tries--)
		{
			if (hello(400)) { return true; }
		}
		return false;
	}
};

#endif /* GTI_FOTA_H */
