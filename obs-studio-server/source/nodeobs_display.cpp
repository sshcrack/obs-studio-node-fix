/******************************************************************************
    Copyright (C) 2016-2019 by Streamlabs (General Workings Inc)

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

******************************************************************************/

#include "nodeobs_display.h"
#include <iostream>
#include <map>
#include <string>
#include "nodeobs_api.h"

#include <graphics/matrix4.h>
#include <graphics/vec4.h>
#include <util/platform.h>

std::vector<std::pair<std::string, std::pair<uint32_t, uint32_t>>> sourcesSize;

extern std::string currentScene; /* defined in OBS_content.cpp */

static const uint32_t grayPaddingArea = 10ul;
std::mutex            OBS::Display::m_displayMtx;

static void RecalculateApectRatioConstrainedSize(
    uint32_t  origW,
    uint32_t  origH,
    uint32_t  sourceW,
    uint32_t  sourceH,
    int32_t&  outX,
    int32_t&  outY,
    uint32_t& outW,
    uint32_t& outH)
{
	double_t sourceAR = double_t(sourceW) / double_t(sourceH);
	double_t origAR   = double_t(origW) / double_t(origH);
	if (origAR > sourceAR) {
		outW = uint32_t(double_t(origH) * sourceAR);
		outH = origH;
	} else {
		outW = origW;
		outH = uint32_t(double_t(origW) * (1.0 / sourceAR));
	}
	outX = (int32_t(origW / 2) - int32_t(outW / 2));
	outY = (int32_t(origH / 2) - int32_t(outH / 2));
}

#ifdef _WIN32
enum class SystemWorkerMessage : uint32_t
{
	CreateWindow  = WM_USER + 0,
	DestroyWindow = WM_USER + 1,
	StopThread    = WM_USER + 2,
};

struct message_answer
{
	HANDLE      event;
	bool        called  = false;
	bool        success = false;
	DWORD       errorCode;
	std::string errorMessage;

	message_answer()
	{
		event = CreateSemaphore(NULL, 0, INT32_MAX, NULL);
	}
	~message_answer()
	{
		CloseHandle(event);
	}

	bool wait()
	{
		return WaitForSingleObject(event, 1) == WAIT_OBJECT_0;
	}

	bool try_wait()
	{
		return WaitForSingleObject(event, 0) == WAIT_OBJECT_0;
	}

	void signal()
	{
		ReleaseSemaphore(event, 1, NULL);
	}
};

struct CreateWindowMessageQuestion
{
	HWND     parentWindow;
	uint32_t width, height;
};

struct CreateWindowMessageAnswer : message_answer
{
	HWND windowHandle;
};

struct DestroyWindowMessageQuestion
{
	HWND window;
};

struct DestroyWindowMessageAnswer : message_answer
{};

static void HandleWin32ErrorMessage(DWORD errorCode)
{
	LPSTR lpErrorStr     = nullptr;
	DWORD dwErrorStrSize = 16;
	DWORD dwErrorStrLen  = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
        NULL,
        errorCode,
        LANG_USER_DEFAULT,
        lpErrorStr,
        dwErrorStrSize,
        NULL);
	std::string exceptionMessage("Unexpected WinAPI error: " + std::string(lpErrorStr, dwErrorStrLen));
	LocalFree(lpErrorStr);
	throw std::system_error(errorCode, std::system_category(), exceptionMessage);
}

void OBS::Display::SystemWorker()
{
	MSG message;
	PeekMessage(&message, NULL, WM_USER, WM_USER, PM_NOREMOVE);

	bool keepRunning = true;
	do {
		BOOL gotMessage = GetMessage(&message, NULL, 0, 0);
		if (gotMessage == 0) {
			continue;
		}
		if (gotMessage == -1) {
			break; // Some sort of error.
		}

		if (message.hwnd != NULL) {
			TranslateMessage(&message);
			DispatchMessage(&message);
			continue;
		}

		switch ((SystemWorkerMessage)message.message) {
		case SystemWorkerMessage::CreateWindow: {
			CreateWindowMessageQuestion* question = reinterpret_cast<CreateWindowMessageQuestion*>(message.wParam);
			CreateWindowMessageAnswer*   answer   = reinterpret_cast<CreateWindowMessageAnswer*>(message.lParam);

			BOOL enabled = FALSE;
			DwmIsCompositionEnabled(&enabled);
			DWORD windowStyle;

			if (IsWindows8OrGreater() || !enabled) {
				windowStyle = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST;
			} else {
				windowStyle = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_COMPOSITED;
			}

			HWND newWindow = CreateWindowEx(
			    windowStyle,
			    TEXT("Win32DisplayClass"),
			    TEXT("SlobsChildWindowPreview"),
			    WS_VISIBLE | WS_POPUP | WS_CHILD,
			    0,
			    0,
			    question->width,
			    question->height,
			    NULL,
			    NULL,
			    NULL,
			    this);

			if (!newWindow) {
				answer->success = false;
				HandleWin32ErrorMessage(GetLastError());
			} else {
				if (IsWindows8OrGreater() || !enabled) {
					SetLayeredWindowAttributes(newWindow, 0, 255, LWA_ALPHA);
				}

				SetParent(newWindow, question->parentWindow);
				answer->windowHandle = newWindow;
				answer->success      = true;
			}

			answer->called = true;
			answer->signal();
			break;
		}
		case SystemWorkerMessage::DestroyWindow: {
			DestroyWindowMessageQuestion* question = reinterpret_cast<DestroyWindowMessageQuestion*>(message.wParam);
			DestroyWindowMessageAnswer*   answer   = reinterpret_cast<DestroyWindowMessageAnswer*>(message.lParam);

			if (!DestroyWindow(question->window)) {
				auto error = GetLastError();

				// We check for error 1400 because if this display is a projector, it is attached to a HTML DOM, so
				// we cannot directly control its destruction since the HTML will probably do this concurrently,
				// the DestroyWindow is allows to fail on this case, a better solution here woul be checking if this
				// display is really a projector and do not attempt to destroy it (let the HTML do it for us).
				if (error != 1400) {
					answer->success = false;
					HandleWin32ErrorMessage(error);
				} else {
					answer->success = true;
				}

			} else {
				answer->success = true;
			}

			answer->called = true;
			answer->signal();
			break;
		}
		case SystemWorkerMessage::StopThread: {
			keepRunning = false;
			break;
		}
		}
	} while (keepRunning);
}
#endif

OBS::Display::Display()
{
#if defined(_WIN32)
	DisplayWndClass();
#elif defined(__APPLE__)
#elif defined(__linux__) || defined(__FreeBSD__)
#endif

#ifdef WIN32
	worker = std::thread(std::bind(&OBS::Display::SystemWorker, this));
#endif
	m_gsInitData.adapter         = 0;
	m_gsInitData.cx              = 0;
	m_gsInitData.cy              = 0;
	m_gsInitData.format          = GS_RGBA;
	m_gsInitData.zsformat        = GS_ZS_NONE;
	m_gsInitData.num_backbuffers = 1;
	m_display                    = nullptr;
	m_source                     = nullptr;
	m_position.first             = 0;
	m_position.second            = 0;

	obs_enter_graphics();
	m_gsSolidEffect = obs_get_base_effect(OBS_EFFECT_SOLID);
	GS::Vertex v(nullptr, nullptr, nullptr, nullptr, nullptr);

	m_boxLine = std::make_unique<GS::VertexBuffer>(6);
	m_boxLine->Resize(6);
	v = m_boxLine->At(0);
	vec3_set(v.position, 0, 0, 0);
	vec4_set(v.uv[0], 0, 0, 0, 0);
	*v.color = 0xFFFFFFFF;
	v        = m_boxLine->At(1);
	vec3_set(v.position, 1, 0, 0);
	vec4_set(v.uv[0], 1, 0, 0, 0);
	*v.color = 0xFFFFFFFF;
	v        = m_boxLine->At(2);
	vec3_set(v.position, 1, 1, 0);
	vec4_set(v.uv[0], 1, 1, 0, 0);
	*v.color = 0xFFFFFFFF;
	v        = m_boxLine->At(3);
	vec3_set(v.position, 0, 1, 0);
	vec4_set(v.uv[0], 0, 1, 0, 0);
	*v.color = 0xFFFFFFFF;
	v        = m_boxLine->At(4);
	vec3_set(v.position, 0, 0, 0);
	vec4_set(v.uv[0], 0, 0, 0, 0);
	*v.color = 0xFFFFFFFF;
	m_boxLine->Update();

	m_boxTris = std::make_unique<GS::VertexBuffer>(4);
	m_boxTris->Resize(4);
	v = m_boxTris->At(0);
	vec3_set(v.position, 0, 0, 0);
	vec4_set(v.uv[0], 0, 0, 0, 0);
	*v.color = 0xFFFFFFFF;
	v        = m_boxTris->At(1);
	vec3_set(v.position, 1, 0, 0);
	vec4_set(v.uv[0], 1, 0, 0, 0);
	*v.color = 0xFFFFFFFF;
	v        = m_boxTris->At(2);
	vec3_set(v.position, 0, 1, 0);
	vec4_set(v.uv[0], 0, 1, 0, 0);
	*v.color = 0xFFFFFFFF;
	v        = m_boxTris->At(3);
	vec3_set(v.position, 1, 1, 0);
	vec4_set(v.uv[0], 1, 1, 0, 0);
	*v.color = 0xFFFFFFFF;
	m_boxTris->Update();

	// Text
	m_textVertices = new GS::VertexBuffer(65535);
	m_textEffect   = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	m_textTexture  = gs_texture_create_from_file((g_moduleDirectory + "/resources/roboto.png").c_str());
	if (!m_textTexture) {
		throw std::runtime_error("couldn't load roboto font");
	}

	obs_leave_graphics();

	SetOutlineColor(26, 230, 168);
	SetGuidelineColor(26, 230, 168);

	UpdatePreviewArea();

	m_drawGuideLines = true;
}

OBS::Display::Display(uint64_t windowHandle, enum obs_video_rendering_mode mode) : Display()
{
#ifdef _WIN32
	CreateWindowMessageQuestion question;
	CreateWindowMessageAnswer   answer;

	question.parentWindow = (HWND)windowHandle;
	question.width        = m_gsInitData.cx;
	question.height       = m_gsInitData.cy;
	while (!PostThreadMessage(
	    GetThreadId(worker.native_handle()),
	    (UINT)SystemWorkerMessage::CreateWindow,
	    reinterpret_cast<intptr_t>(&question),
	    reinterpret_cast<intptr_t>(&answer))) {
		Sleep(0);
	}

	if (!answer.try_wait()) {
		while (!answer.wait()) {
			if (answer.called)
				break;
			Sleep(0);
		}
	}

	if (!answer.success) {
		throw std::system_error(answer.errorCode, std::system_category(), answer.errorMessage);
	}

	m_ourWindow              = answer.windowHandle;
	m_parentWindow           = reinterpret_cast<HWND>(windowHandle);
	m_gsInitData.window.hwnd = reinterpret_cast<void*>(m_ourWindow);
#endif
	m_displayMtx.lock();
	m_display = obs_display_create(&m_gsInitData, 0x0);

	if (!m_display) {
		blog(LOG_INFO, "Failed to create the display");
		throw std::runtime_error("unable to create display");
	}

	m_renderingMode = mode;

	obs_display_add_draw_callback(m_display, DisplayCallback, this);
	m_displayMtx.unlock();
}

OBS::Display::Display(uint64_t windowHandle, enum obs_video_rendering_mode mode, std::string sourceName)
    : Display(windowHandle, mode)
{
	m_source = obs_get_source_by_name(sourceName.c_str());
	obs_source_inc_showing(m_source);
}

OBS::Display::~Display()
{
	m_displayMtx.lock();
	obs_display_remove_draw_callback(m_display, DisplayCallback, this);

	if (m_source) {
		obs_source_dec_showing(m_source);
		obs_source_release(m_source);
	}

	if (m_textVertices) {
		delete m_textVertices;
	}

	if (m_textTexture) {
		obs_enter_graphics();
		gs_texture_destroy(m_textTexture);
		obs_leave_graphics();
	}

	m_boxLine = nullptr;
	m_boxTris = nullptr;

	if (m_display)
		obs_display_destroy(m_display);
	m_displayMtx.unlock();

#ifdef _WIN32
	DestroyWindowMessageQuestion question;
	DestroyWindowMessageAnswer   answer;

	question.window = m_ourWindow;
	PostThreadMessage(
	    GetThreadId(worker.native_handle()),
	    (UINT)SystemWorkerMessage::DestroyWindow,
	    reinterpret_cast<intptr_t>(&question),
	    reinterpret_cast<intptr_t>(&answer));

	if (!answer.try_wait()) {
		while (!answer.wait()) {
			if (answer.called)
				break;
			Sleep(0);
		}
	}

	if (!answer.success) {
		std::cerr << "OBS::Display::~Display: " << answer.errorMessage << std::endl;
	}

	PostThreadMessage(GetThreadId(worker.native_handle()), (UINT)SystemWorkerMessage::StopThread, NULL, NULL);

	if (worker.joinable())
		worker.join();
#endif
}

void OBS::Display::SetPosition(uint32_t x, uint32_t y)
{
#if defined(_WIN32)
	// Store new position.
	m_position.first  = x;
	m_position.second = y;

	if (m_source != NULL) {
		std::string msg =
		    "<" + std::string(__FUNCTION__) + "> Adjusting display position for source %s to %ldx%ld. hwnd %d";
		blog(LOG_DEBUG, msg.c_str(), obs_source_get_name(m_source), x, y, m_ourWindow);
	}

	SetWindowPos(
	    m_ourWindow,
	    NULL,
	    m_position.first,
	    m_position.second,
	    m_gsInitData.cx,
	    m_gsInitData.cy,
	    SWP_NOCOPYBITS | SWP_NOSIZE | SWP_NOACTIVATE);
#endif
}

std::pair<uint32_t, uint32_t> OBS::Display::GetPosition()
{
	return m_position;
}

bool isNewerThanWindows7()
{
#ifdef WIN32
	static bool versionIsHigherThan7 = false;
	static bool versionIsChecked     = false;
	if (!versionIsChecked) {
		OSVERSIONINFO osvi;
		BOOL          bIsWindowsXPorLater;

		ZeroMemory(&osvi, sizeof(OSVERSIONINFO));
		osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);

		GetVersionEx(&osvi);

		versionIsHigherThan7 = ((osvi.dwMajorVersion > 6) || ((osvi.dwMajorVersion == 6) && (osvi.dwMinorVersion > 1)));

		versionIsChecked = true;
	}
	return versionIsHigherThan7;
#else
	return false;
#endif
}

void OBS::Display::setSizeCall(int step)
{
	int         use_x, use_y;
	int         use_width, use_height;
	const float presizes[] = {1, 1.05, 1.25, 1.5, 2.0, 3.0};

	switch (step) {
	case -1:
		use_width  = m_gsInitData.cx;
		use_height = m_gsInitData.cy;
		use_x      = m_position.first;
		use_y      = m_position.second;
		break;
	case 0:
		use_width  = m_gsInitData.cx - 2;
		use_height = m_gsInitData.cy - 2;
		use_x      = m_position.first + 1;
		use_y      = m_position.second + 1;
		break;
	case 1:
	case 2:
	case 3:
	case 4:
	case 5:
		use_width  = float(m_gsInitData.cx) / presizes[step];
		use_height = float(m_gsInitData.cy) / presizes[step];
		use_x      = m_position.first + (m_gsInitData.cx - use_width) / 2;
		use_y      = m_position.second + (m_gsInitData.cy - use_height) / 2;
		break;
	}

	BOOL ret = true;
	// Resize Window
#if defined(_WIN32)
	if (step > 0) {
		ret = SetWindowPos(
		    m_ourWindow,
		    NULL,
		    use_x,
		    use_y,
		    use_width,
		    use_height,
		    SWP_NOCOPYBITS | SWP_NOACTIVATE | SWP_NOZORDER | SWP_HIDEWINDOW);
	} else {
		ret = SetWindowPos(
		    m_ourWindow,
		    NULL,
		    use_x,
		    use_y,
		    use_width,
		    use_height,
		    SWP_NOCOPYBITS | SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
		if (ret)
			RedrawWindow(m_ourWindow, NULL, NULL, RDW_ERASE | RDW_INVALIDATE);
	}
#elif defined(__APPLE__)
#elif defined(__linux__) || defined(__FreeBSD__)
#endif

	if (step >= 0 && ret) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		std::thread{&OBS::Display::setSizeCall, this, step - 1}.detach();
	}
};

void OBS::Display::SetSize(uint32_t width, uint32_t height)
{
#ifdef WIN32
	if (m_source != NULL) {
		std::string msg =
		    "<" + std::string(__FUNCTION__) + "> Adjusting display size for source %s to %ldx%ld. hwnd %d";
		blog(LOG_DEBUG, msg.c_str(), obs_source_get_name(m_source), width, height, m_ourWindow);
	}

	m_gsInitData.cx = width;
	m_gsInitData.cy = height;

	if (width == 0 || height == 0 || isNewerThanWindows7()) {
		setSizeCall(-1);
	} else {
		setSizeCall(4);
	}

	// Resize Display
	obs_display_resize(m_display, m_gsInitData.cx, m_gsInitData.cy);

	// Store new size.
	UpdatePreviewArea();
#endif
}

std::pair<uint32_t, uint32_t> OBS::Display::GetSize()
{
	return std::make_pair(m_gsInitData.cx, m_gsInitData.cy);
}

std::pair<int32_t, int32_t> OBS::Display::GetPreviewOffset()
{
	return m_previewOffset;
}

std::pair<uint32_t, uint32_t> OBS::Display::GetPreviewSize()
{
	return m_previewSize;
}

void OBS::Display::SetDrawUI(bool v /*= true*/)
{
	m_shouldDrawUI = v;
}

bool OBS::Display::GetDrawUI()
{
	return m_shouldDrawUI;
}

void OBS::Display::SetPaddingColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a /*= 255u*/)
{
	m_paddingColor[0] = float_t(r) / 255.0f;
	m_paddingColor[1] = float_t(g) / 255.0f;
	m_paddingColor[2] = float_t(b) / 255.0f;
	m_paddingColor[3] = float_t(a) / 255.0f;
}

void OBS::Display::SetPaddingSize(uint32_t pixels)
{
	m_paddingSize = pixels;
	UpdatePreviewArea();
}

void OBS::Display::SetBackgroundColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a /*= 255u*/)
{
	m_backgroundColor = a << 24 | b << 16 | g << 8 | r;
}

void OBS::Display::SetOutlineColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a /*= 255u*/)
{
	m_outlineColor = a << 24 | b << 16 | g << 8 | r;
}

void OBS::Display::SetGuidelineColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a /*= 255u*/)
{
	m_guidelineColor = a << 24 | b << 16 | g << 8 | r;
}

void OBS::Display::SetResizeBoxOuterColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a /*= 255u*/)
{
	m_resizeOuterColor = a << 24 | b << 16 | g << 8 | r;
}

void OBS::Display::SetResizeBoxInnerColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a /*= 255u*/)
{
	m_resizeInnerColor = a << 24 | b << 16 | g << 8 | r;
}

static void
    DrawGlyph(GS::VertexBuffer* vb, float_t x, float_t y, float_t scale, float_t depth, char glyph, uint32_t color)
{
	float uvX = 0, uvY = 0, uvO = 1.0 / 4.0;
	switch (glyph) {
	default:
		return;
		break;
	case '1':
		uvX = 0;
		uvY = 0;
		break;
	case '2':
		uvX = uvO;
		uvY = 0;
		break;
	case '3':
		uvX = uvO * 2;
		uvY = 0;
		break;
	case '4':
		uvX = uvO * 3;
		uvY = 0;
		break;
	case '5':
		uvX = 0;
		uvY = uvO * 1;
		break;
	case '6':
		uvX = uvO;
		uvY = uvO * 1;
		break;
	case '7':
		uvX = uvO * 2;
		uvY = uvO * 1;
		break;
	case '8':
		uvX = uvO * 3;
		uvY = uvO * 1;
		break;
	case '9':
		uvX = 0;
		uvY = uvO * 2;
		break;
	case '0':
		uvX = uvO;
		uvY = uvO * 2;
		break;
	case 'p':
		uvX = uvO * 2;
		uvY = uvO * 2;
		break;
	case 'x':
		uvX = uvO * 3;
		uvY = uvO * 2;
		break;
	}

	GS::Vertex v(nullptr, nullptr, nullptr, nullptr, nullptr);
	size_t     bs = vb->Size();
	vb->Resize(uint32_t(bs + 6));

	// Top Left
	v = vb->At(uint32_t(bs + 0));
	vec3_set(v.position, x, y, depth);
	vec4_set(v.uv[0], uvX, uvY, 0, 0);
	*v.color = color;
	// Top Right
	v = vb->At(uint32_t(bs + 1));
	vec3_set(v.position, x + scale, y, depth);
	vec4_set(v.uv[0], uvX + uvO, uvY, 0, 0);
	*v.color = color;
	// Bottom Left
	v = vb->At(uint32_t(bs + 2));
	vec3_set(v.position, x, y + scale * 2, depth);
	vec4_set(v.uv[0], uvX, uvY + uvO, 0, 0);
	*v.color = color;

	// Top Right
	v = vb->At(uint32_t(bs + 3));
	vec3_set(v.position, x + scale, y, depth);
	vec4_set(v.uv[0], uvX + uvO, uvY, 0, 0);
	*v.color = color;
	// Bottom Left
	v = vb->At(uint32_t(bs + 4));
	vec3_set(v.position, x, y + scale * 2, depth);
	vec4_set(v.uv[0], uvX, uvY + uvO, 0, 0);
	*v.color = color;
	// Bottom Right
	v = vb->At(uint32_t(bs + 5));
	vec3_set(v.position, x + scale, y + scale * 2, depth);
	vec4_set(v.uv[0], uvX + uvO, uvY + uvO, 0, 0);
	*v.color = color;
}

#define HANDLE_RADIUS 5.0f
#define HANDLE_DIAMETER 10.0f

inline bool CloseFloat(float a, float b, float epsilon = 0.01)
{
	return abs(a - b) <= epsilon;
}

inline void DrawOutline(OBS::Display* dp, matrix4& mtx, obs_transform_info& info)
{
	gs_matrix_push();
	gs_matrix_set(&mtx);
	gs_draw(GS_LINESTRIP, 0, 0);
	gs_matrix_pop();
}

inline void DrawBoxAt(OBS::Display* dp, float_t x, float_t y, matrix4& mtx)
{
	gs_matrix_push();

	vec3 pos = {x, y, 0.0f};
	vec3_transform(&pos, &pos, &mtx);

	vec3 offset = {-HANDLE_RADIUS, -HANDLE_RADIUS, 0.0f};
	offset.x *= dp->m_previewToWorldScale.x;
	offset.y *= dp->m_previewToWorldScale.y;

	gs_matrix_translate(&pos);
	gs_matrix_translate(&offset);
	gs_matrix_scale3f(
	    HANDLE_DIAMETER * dp->m_previewToWorldScale.x, HANDLE_DIAMETER * dp->m_previewToWorldScale.y, 1.0f);

	gs_draw(GS_LINESTRIP, 0, 0);
	gs_matrix_pop();
}

inline void DrawSquareAt(OBS::Display* dp, float_t x, float_t y, matrix4& mtx)
{
	gs_matrix_push();

	vec3 pos = {x, y, 0.0f};
	vec3_transform(&pos, &pos, &mtx);

	vec3 offset = {-HANDLE_RADIUS, -HANDLE_RADIUS, 0.0f};
	offset.x *= dp->m_previewToWorldScale.x;
	offset.y *= dp->m_previewToWorldScale.y;

	gs_matrix_translate(&pos);
	gs_matrix_translate(&offset);
	gs_matrix_scale3f(
	    HANDLE_DIAMETER * dp->m_previewToWorldScale.x, HANDLE_DIAMETER * dp->m_previewToWorldScale.y, 1.0f);

	gs_draw(GS_TRISTRIP, 0, 0);
	gs_matrix_pop();
}

inline void DrawGuideline(OBS::Display* dp, float_t x, float_t y, matrix4& mtx)
{
	gs_rect rect;
	rect.x  = dp->GetPreviewOffset().first;
	rect.y  = dp->GetPreviewOffset().second;
	rect.cx = dp->GetPreviewSize().first;
	rect.cy = dp->GetPreviewSize().second;

	gs_set_scissor_rect(&rect);
	gs_matrix_push();

	vec3 center = {0.5, 0.5, 0.0f};
	vec3_transform(&center, &center, &mtx);

	vec3 pos = {x, y, 0.0f};
	vec3_transform(&pos, &pos, &mtx);

	vec3 normal;
	vec3_sub(&normal, &center, &pos);
	vec3_norm(&normal, &normal);

	gs_matrix_translate(&pos);

	vec3 up = {0, 1.0, 0};
	vec3 dn = {0, -1.0, 0};
	vec3 lt = {-1.0, 0, 0};
	vec3 rt = {1.0, 0, 0};

	if (vec3_dot(&up, &normal) > 0.5f) {
		// Dominantly looking up.
		gs_matrix_rotaa4f(0, 0, 1, RAD(-90.0f));
	} else if (vec3_dot(&dn, &normal) > 0.5f) {
		// Dominantly looking down.
		gs_matrix_rotaa4f(0, 0, 1, RAD(90.0f));
	} else if (vec3_dot(&lt, &normal) > 0.5f) {
		// Dominantly looking left.
		gs_matrix_rotaa4f(0, 0, 1, RAD(0.0f));
	} else if (vec3_dot(&rt, &normal) > 0.5f) {
		// Dominantly looking right.
		gs_matrix_rotaa4f(0, 0, 1, RAD(180.0f));
	}

	gs_matrix_scale3f(65535, 65535, 65535);

	gs_draw(GS_LINES, 0, 2);

	gs_matrix_pop();
	gs_set_scissor_rect(nullptr);
}

bool OBS::Display::DrawSelectedSource(obs_scene_t* scene, obs_sceneitem_t* item, void* param)
{
	// This is partially code from OBS Studio. See window-basic-preview.cpp in obs-studio for copyright/license.
	if (obs_sceneitem_locked(item))
		return true;

	obs_source_t* itemSource  = obs_sceneitem_get_source(item);
	uint32_t      flags       = obs_source_get_output_flags(itemSource);
	bool          isOnlyAudio = (flags & OBS_SOURCE_VIDEO) == 0;

	obs_source_t* sceneSource = obs_scene_get_source(scene);

	uint32_t sceneWidth  = obs_source_get_width(sceneSource);
	uint32_t sceneHeight = obs_source_get_height(sceneSource);
	uint32_t itemWidth   = obs_source_get_width(itemSource);
	uint32_t itemHeight  = obs_source_get_height(itemSource);

	if (!obs_sceneitem_selected(item) || isOnlyAudio || ((itemWidth <= 0) && (itemHeight <= 0)))
		return true;

	matrix4 boxTransform;
	matrix4 invBoxTransform;
	obs_sceneitem_get_box_transform(item, &boxTransform);
	matrix4_inv(&invBoxTransform, &boxTransform);

	{
		vec3 bounds[] = {
		    {{{0.f, 0.f, 0.f}}},
		    {{{1.f, 0.f, 0.f}}},
		    {{{0.f, 1.f, 0.f}}},
		    {{{1.f, 1.f, 0.f}}},
		};
		bool visible = std::all_of(std::begin(bounds), std::end(bounds), [&](const vec3& b) {
			vec3 pos;
			vec3_transform(&pos, &b, &boxTransform);
			vec3_transform(&pos, &pos, &invBoxTransform);
			return CloseFloat(pos.x, b.x) && CloseFloat(pos.y, b.y);
		});

		if (!visible)
			return true;
	}

	OBS::Display* dp = reinterpret_cast<OBS::Display*>(param);

	vec4         color;
	gs_effect_t* solid       = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t* solid_color = gs_effect_get_param_by_name(solid, "color");

	obs_transform_info info;
	obs_sceneitem_get_info(item, &info);

	gs_load_vertexbuffer(dp->m_boxLine->Update(false));
	vec4_set(
	    &color,
	    (dp->m_outlineColor & 0xFF) / 255.0f,
	    ((dp->m_outlineColor & 0xFF00) >> 8) / 255.0f,
	    ((dp->m_outlineColor & 0xFF0000) >> 16) / 255.0f,
	    ((dp->m_outlineColor & 0xFF000000) >> 24) / 255.0f);
	gs_effect_set_vec4(solid_color, &color);
	DrawOutline(dp, boxTransform, info);

	gs_load_vertexbuffer(dp->m_boxTris->Update(false));
	vec4_set(
	    &color,
	    (dp->m_resizeInnerColor & 0xFF) / 255.0f,
	    ((dp->m_resizeInnerColor & 0xFF00) >> 8) / 255.0f,
	    ((dp->m_resizeInnerColor & 0xFF0000) >> 16) / 255.0f,
	    ((dp->m_resizeInnerColor & 0xFF000000) >> 24) / 255.0f);
	gs_effect_set_vec4(solid_color, &color);
	DrawSquareAt(dp, 0, 0, boxTransform);
	DrawSquareAt(dp, 1, 0, boxTransform);
	DrawSquareAt(dp, 0, 1, boxTransform);
	DrawSquareAt(dp, 1, 1, boxTransform);
	DrawSquareAt(dp, 0.5, 0, boxTransform);
	DrawSquareAt(dp, 0.5, 1, boxTransform);
	DrawSquareAt(dp, 0, 0.5, boxTransform);
	DrawSquareAt(dp, 1, 0.5, boxTransform);

	gs_load_vertexbuffer(dp->m_boxLine->Update(false));
	vec4_set(
	    &color,
	    (dp->m_resizeOuterColor & 0xFF) / 255.0f,
	    ((dp->m_resizeOuterColor & 0xFF00) >> 8) / 255.0f,
	    ((dp->m_resizeOuterColor & 0xFF0000) >> 16) / 255.0f,
	    ((dp->m_resizeOuterColor & 0xFF000000) >> 24) / 255.0f);
	gs_effect_set_vec4(solid_color, &color);
	DrawBoxAt(dp, 0, 0, boxTransform);
	DrawBoxAt(dp, 1, 0, boxTransform);
	DrawBoxAt(dp, 0, 1, boxTransform);
	DrawBoxAt(dp, 1, 1, boxTransform);
	DrawBoxAt(dp, 0.5, 0, boxTransform);
	DrawBoxAt(dp, 0.5, 1, boxTransform);
	DrawBoxAt(dp, 0, 0.5, boxTransform);
	DrawBoxAt(dp, 1, 0.5, boxTransform);

	if (dp->m_drawGuideLines) {
		vec4_set(
		    &color,
		    (dp->m_guidelineColor & 0xFF) / 255.0f,
		    ((dp->m_guidelineColor & 0xFF00) >> 8) / 255.0f,
		    ((dp->m_guidelineColor & 0xFF0000) >> 16) / 255.0f,
		    ((dp->m_guidelineColor & 0xFF000000) >> 24) / 255.0f);
		gs_effect_set_vec4(solid_color, &color);
		DrawGuideline(dp, 0.5, 0, boxTransform);
		DrawGuideline(dp, 0.5, 1, boxTransform);
		DrawGuideline(dp, 0, 0.5, boxTransform);
		DrawGuideline(dp, 1, 0.5, boxTransform);

		// TEXT RENDERING
		// THIS DESPERATELY NEEDS TO BE REWRITTEN INTO SHADER CODE
		// DO SO WHENEVER...
		GS::Vertex v(nullptr, nullptr, nullptr, nullptr, nullptr);

		matrix4 itemMatrix, sceneToView;
		obs_sceneitem_get_box_transform(item, &itemMatrix);
		matrix4_identity(&sceneToView);
		sceneToView.x.x = dp->m_worldToPreviewScale.x;
		sceneToView.y.y = dp->m_worldToPreviewScale.y;

		// Retrieve actual corner and edge positions.
		vec3 edge[4], center;
		{
			vec3_set(&edge[0], 0, 0.5, 0);
			vec3_transform(&edge[0], &edge[0], &itemMatrix);
			vec3_set(&edge[1], 0.5, 0, 0);
			vec3_transform(&edge[1], &edge[1], &itemMatrix);
			vec3_set(&edge[2], 1, 0.5, 0);
			vec3_transform(&edge[2], &edge[2], &itemMatrix);
			vec3_set(&edge[3], 0.5, 1, 0);
			vec3_transform(&edge[3], &edge[3], &itemMatrix);

			vec3_set(&center, 0.5, 0.5, 0);
			vec3_transform(&center, &center, &itemMatrix);
			;
		}

		std::vector<char> buf(8);
		float_t           pt = 8 * dp->m_previewToWorldScale.y;
		for (size_t n = 0; n < 4; n++) {
			bool isIn = (edge[n].x >= 0) && (edge[n].x < sceneWidth) && (edge[n].y >= 0) && (edge[n].y < sceneHeight);

			if (!isIn)
				continue;

			vec3 alignLeft = {-1, 0, 0};
			vec3 alignTop  = {0, -1, 0};

			vec3 temp;
			vec3_sub(&temp, &edge[n], &center);
			vec3_norm(&temp, &temp);
			float left = vec3_dot(&temp, &alignLeft), top = vec3_dot(&temp, &alignTop);
			if (left > 0.5) { // LEFT
				float_t dist = edge[n].x;
				if (dist > (pt * 4)) {
					size_t  len    = (size_t)snprintf(buf.data(), buf.size(), "%ld px", (uint32_t)dist);
					float_t offset = float((pt * len) / 2.0);

					for (size_t p = 0; p < len; p++) {
						char v = buf.data()[p];
						DrawGlyph(
						    dp->m_textVertices,
						    (edge[n].x / 2) - offset + (p * pt),
						    edge[n].y - pt * 2,
						    pt,
						    0,
						    v,
						    dp->m_guidelineColor);
					}
				}
			} else if (left < -0.5) { // RIGHT
				float_t dist = sceneWidth - edge[n].x;
				if (dist > (pt * 4)) {
					size_t  len    = (size_t)snprintf(buf.data(), buf.size(), "%ld px", (uint32_t)dist);
					float_t offset = float((pt * len) / 2.0);

					for (size_t p = 0; p < len; p++) {
						char v = buf.data()[p];
						DrawGlyph(
						    dp->m_textVertices,
						    edge[n].x + (dist / 2) - offset + (p * pt),
						    edge[n].y - pt * 2,
						    pt,
						    0,
						    v,
						    dp->m_guidelineColor);
					}
				}
			} else if (top > 0.5) { // UP
				float_t dist = edge[n].y;
				if (dist > pt) {
					size_t  len    = (size_t)snprintf(buf.data(), buf.size(), "%ld px", (uint32_t)dist);
					float_t offset = float((pt * len) / 2.0);

					for (size_t p = 0; p < len; p++) {
						char v = buf.data()[p];
						DrawGlyph(
						    dp->m_textVertices,
						    edge[n].x + (p * pt),
						    edge[n].y - (dist / 2) - pt,
						    pt,
						    0,
						    v,
						    dp->m_guidelineColor);
					}
				}
			} else if (top < -0.5) { // DOWN
				float_t dist = sceneHeight - edge[n].y;
				if (dist > (pt * 4)) {
					size_t  len    = (size_t)snprintf(buf.data(), buf.size(), "%ld px", (uint32_t)dist);
					float_t offset = float((pt * len) / 2.0);

					for (size_t p = 0; p < len; p++) {
						char v = buf.data()[p];
						DrawGlyph(
						    dp->m_textVertices,
						    edge[n].x + (p * pt),
						    edge[n].y + (dist / 2) - pt,
						    pt,
						    0,
						    v,
						    dp->m_guidelineColor);
					}
				}
			}
		}
	}

	return true;
}

void OBS::Display::DisplayCallback(void* displayPtr, uint32_t cx, uint32_t cy)
{
	Display*        dp          = static_cast<Display*>(displayPtr);
	gs_effect_t*    solid       = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t*    solid_color = gs_effect_get_param_by_name(solid, "color");
	gs_technique_t* solid_tech  = gs_effect_get_technique(solid, "Solid");
	vec4            color;

	dp->UpdatePreviewArea();

	// Get proper source/base size.
	uint32_t sourceW, sourceH;
	if (dp->m_source) {
		sourceW = obs_source_get_width(dp->m_source);
		sourceH = obs_source_get_height(dp->m_source);
		if (sourceW == 0)
			sourceW = 1;
		if (sourceH == 0)
			sourceH = 1;
	} else {
		obs_video_info ovi;
		obs_get_video_info(&ovi);

		sourceW = ovi.base_width;
		sourceH = ovi.base_height;
		if (sourceW == 0)
			sourceW = 1;
		if (sourceH == 0)
			sourceH = 1;
	}

	gs_viewport_push();
	gs_projection_push();

	gs_ortho(0.0f, float(sourceW), 0.0f, float(sourceH), -100.0f, 100.0f);

	gs_set_viewport(
	    dp->m_previewOffset.first, dp->m_previewOffset.second, dp->m_previewSize.first, dp->m_previewSize.second);

	// Padding
	vec4_set(&color, dp->m_paddingColor[0], dp->m_paddingColor[1], dp->m_paddingColor[2], dp->m_paddingColor[3]);
	gs_clear(GS_CLEAR_COLOR | GS_CLEAR_DEPTH | GS_CLEAR_STENCIL, &color, 100, 0);

	// Background
	if (dp->m_boxTris) {
		vec4_set(
		    &color,
		    ((dp->m_backgroundColor & 0xFF)) / 255.0f,
		    ((dp->m_backgroundColor & 0xFF00) >> 8) / 255.0f,
		    ((dp->m_backgroundColor & 0xFF0000) >> 16) / 255.0f,
		    ((dp->m_backgroundColor & 0xFF000000) >> 24) / 255.0f);
		gs_effect_set_vec4(solid_color, &color);

		gs_technique_begin(solid_tech);
		gs_technique_begin_pass(solid_tech, 0);

		gs_matrix_push();
		gs_matrix_identity();
		gs_matrix_scale3f(float(sourceW), float(sourceH), 1.0f);

		gs_load_vertexbuffer(dp->m_boxTris->Update(false));
		gs_draw(GS_TRISTRIP, 0, 0);

		gs_matrix_pop();

		gs_technique_end_pass(solid_tech);
		gs_technique_end(solid_tech);
	}

	// Source Rendering
	obs_source_t* source = NULL;
	if (dp->m_source) {
		/* If the the source is a transition it means this display 
		 * is for Studio Mode and that the scene it contains is a 
		 * duplicate of the current scene, apply selective recording
		 * layer rendering if it is enabled */
		if (obs_get_multiple_rendering() && obs_source_get_type(dp->m_source) == OBS_SOURCE_TYPE_TRANSITION)
			obs_set_video_rendering_mode(dp->m_renderingMode);

		obs_source_video_render(dp->m_source);
		/* If we want to draw guidelines, we need a scene,
		 * not a transition. This may not be a scene which
		 * we'll check later. */
		if (obs_source_get_type(dp->m_source) == OBS_SOURCE_TYPE_TRANSITION) {
			source = obs_transition_get_active_source(dp->m_source);
		} else {
			source = dp->m_source;
			obs_source_addref(source);
		}
	} else {
		switch (dp->m_renderingMode) {
		case OBS_MAIN_VIDEO_RENDERING:
			obs_render_main_texture();
			break;
		case OBS_STREAMING_VIDEO_RENDERING:
			obs_render_streaming_texture();
			break;
		case OBS_RECORDING_VIDEO_RENDERING:
			obs_render_recording_texture();
			break;
		}

		/* Here we assume that channel 0 holds the primary transition.
		* We also assume that the active source within that transition is
		* the scene that we need */
		obs_source_t* transition = obs_get_output_source(0);
		source                   = obs_transition_get_active_source(transition);
		obs_source_release(transition);
	}

	if (dp->m_shouldDrawUI == true) {
		// Display-Aligned Drawing
		vec2 tlCorner = {(float)-dp->m_previewOffset.first, (float)-dp->m_previewOffset.second};
		vec2 brCorner = {(float)(cx - dp->m_previewOffset.first), (float)(cy - dp->m_previewOffset.second)};
		vec2_mul(&tlCorner, &tlCorner, &dp->m_previewToWorldScale);
		vec2_mul(&brCorner, &brCorner, &dp->m_previewToWorldScale);

		gs_ortho(tlCorner.x, brCorner.x, tlCorner.y, brCorner.y, -100.0f, 100.0f);
		gs_reset_viewport();

		obs_scene_t* scene = obs_scene_from_source(source);

		/* This should work for both individual sources 
		 * that are actually scenes and our main transition scene */

		if (scene) {
			dp->m_textVertices->Resize(0);

			gs_technique_begin(solid_tech);
			gs_technique_begin_pass(solid_tech, 0);

			obs_scene_enum_items(scene, DrawSelectedSource, dp);

			gs_technique_end_pass(solid_tech);
			gs_technique_end(solid_tech);

			// Text Rendering
			if (dp->m_textVertices->Size() > 0) {
				gs_vertbuffer_t* vb = dp->m_textVertices->Update();
				while (gs_effect_loop(dp->m_textEffect, "Draw")) {
					gs_effect_set_texture(gs_effect_get_param_by_name(dp->m_textEffect, "image"), dp->m_textTexture);
					gs_load_vertexbuffer(vb);
					gs_load_indexbuffer(nullptr);
					gs_draw(GS_TRIS, 0, (uint32_t)dp->m_textVertices->Size());
				}
			}
		}
	}

	obs_source_release(source);
	gs_projection_pop();
	gs_viewport_pop();
}

void OBS::Display::UpdatePreviewArea()
{
	int32_t  offsetX = 0, offsetY = 0;
	uint32_t sourceW, sourceH;
	if (m_source) {
		sourceW = obs_source_get_width(m_source);
		sourceH = obs_source_get_height(m_source);
	} else {
		obs_video_info ovi;
		obs_get_video_info(&ovi);

		sourceW = ovi.base_width;
		sourceH = ovi.base_height;
	}

	if (sourceW == 0)
		sourceW = 1;
	if (sourceH == 0)
		sourceH = 1;

	RecalculateApectRatioConstrainedSize(
	    m_gsInitData.cx,
	    m_gsInitData.cy,
	    sourceW,
	    sourceH,
	    m_previewOffset.first,
	    m_previewOffset.second,
	    m_previewSize.first,
	    m_previewSize.second);

	offsetX = m_paddingSize;
	offsetY = float_t(offsetX) * float_t(sourceH) / float_t(sourceW);

	m_previewOffset.first += offsetX;
	m_previewSize.first -= offsetX * 2;

	if (m_previewSize.second <= offsetY * 2) {
		m_previewOffset.second = (m_previewOffset.second - 1) / 2;
		m_previewSize.second   = 1;
	} else {
		m_previewOffset.second += offsetY;
		m_previewSize.second -= offsetY * 2;
	}

	m_worldToPreviewScale.x = float_t(m_previewSize.first) / float_t(sourceW);
	m_worldToPreviewScale.y = float_t(m_previewSize.second) / float_t(sourceH);
	m_previewToWorldScale.x = float_t(sourceW) / float_t(m_previewSize.first);
	m_previewToWorldScale.y = float_t(sourceH) / float_t(m_previewSize.second);
}

#if defined(_WIN32)

bool OBS::Display::DisplayWndClassRegistered;

WNDCLASSEX OBS::Display::DisplayWndClassObj;

ATOM OBS::Display::DisplayWndClassAtom;

void OBS::Display::DisplayWndClass()
{
	if (DisplayWndClassRegistered)
		return;

	DisplayWndClassObj.cbSize = sizeof(WNDCLASSEX);
	DisplayWndClassObj.style  = CS_OWNDC | CS_NOCLOSE | CS_HREDRAW
	                           | CS_VREDRAW; // CS_DBLCLKS | CS_HREDRAW | CS_NOCLOSE | CS_VREDRAW | CS_OWNDC;
	DisplayWndClassObj.lpfnWndProc   = DisplayWndProc;
	DisplayWndClassObj.cbClsExtra    = 0;
	DisplayWndClassObj.cbWndExtra    = 0;
	DisplayWndClassObj.hInstance     = NULL; // HINST_THISCOMPONENT;
	DisplayWndClassObj.hIcon         = NULL;
	DisplayWndClassObj.hCursor       = NULL;
	DisplayWndClassObj.hbrBackground = NULL;
	DisplayWndClassObj.lpszMenuName  = NULL;
	DisplayWndClassObj.lpszClassName = TEXT("Win32DisplayClass");
	DisplayWndClassObj.hIconSm       = NULL;

	DisplayWndClassAtom = RegisterClassEx(&DisplayWndClassObj);
	if (DisplayWndClassAtom == NULL) {
		HandleWin32ErrorMessage(GetLastError());
	}

	DisplayWndClassRegistered = true;
}

LRESULT CALLBACK OBS::Display::DisplayWndProc(_In_ HWND hwnd, _In_ UINT uMsg, _In_ WPARAM wParam, _In_ LPARAM lParam)
{
	OBS::Display* self = nullptr;
	switch (uMsg) {
	case WM_NCHITTEST:
		return HTTRANSPARENT;
	}

	return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

#endif

bool OBS::Display::GetDrawGuideLines(void)
{
	return m_drawGuideLines;
}

void OBS::Display::SetDrawGuideLines(bool drawGuideLines)
{
	m_drawGuideLines = drawGuideLines;
}
