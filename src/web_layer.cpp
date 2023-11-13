#include <algorithm>
#include <atomic>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <random>
#include <regex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <include/cef_parser.h>

#include <include/cef_app.h>
#include <include/cef_browser.h>
#include <include/cef_client.h>
#include <include/cef_request_context_handler.h>

#include "composition.h"
#include "util.h"

struct WebView;
struct FrameBuffer;

static bool _cef_thread;
static uint32_t _zoom_phase = 1;

CefRefPtr<CefV8Value> CefValueToCefV8Value(const CefRefPtr<CefValue>& value)
{
	CefRefPtr<CefV8Value> result;
	switch (value->GetType())
	{
		case VTYPE_INVALID:
		case VTYPE_NULL:
		{
			result = CefV8Value::CreateNull();
		}
		break;
		case VTYPE_BOOL:
		{
			result = CefV8Value::CreateBool(value->GetBool());
		}
		break;
		case VTYPE_INT:
		{
			result = CefV8Value::CreateInt(value->GetInt());
		}
		break;
		case VTYPE_DOUBLE:
		{
			result = CefV8Value::CreateDouble(value->GetDouble());
		}
		break;
		case VTYPE_STRING:
		{
			result = CefV8Value::CreateString(value->GetString());
		}
		break;
		case VTYPE_BINARY:
		{
			result = CefV8Value::CreateNull();
		}
		break;
		case VTYPE_DICTIONARY:
		{
			result = CefV8Value::CreateObject(nullptr, nullptr);
			const auto dict = value->GetDictionary();
			CefDictionaryValue::KeyList keys;
			dict->GetKeys(keys);
			for (auto i = 0U; i < keys.size(); i++)
			{
				auto key = keys[i];
				result->SetValue(key, CefValueToCefV8Value(dict->GetValue(key)), V8_PROPERTY_ATTRIBUTE_NONE);
			}
		}
		break;
		case VTYPE_LIST:
		{
			const auto list = value->GetList();
			const auto size = list->GetSize();
			result = CefV8Value::CreateArray(int(size));
			for (auto i = 0U; i < size; i++)
			{
				result->SetValue(i, CefValueToCefV8Value(list->GetValue(i)));
			}
		}
		break;
	}
	return result;
}

static CefRefPtr<CefValue> CefV8ValueToCefValue(const CefRefPtr<CefV8Value>& valueV8)
{
	auto value = CefValue::Create();
	if (!valueV8->IsValid())
	{
		return value;
	}

	if (valueV8->IsBool())
	{
		value->SetBool(valueV8->GetBoolValue());
		return value;
	}

	if (valueV8->IsInt())
	{
		value->SetInt(valueV8->GetIntValue());
		return value;
	}

	if (valueV8->IsDouble())
	{
		value->SetDouble(valueV8->GetDoubleValue());
		return value;
	}

	if (valueV8->IsString())
	{
		value->SetString(valueV8->GetStringValue());
		return value;
	}

	if (valueV8->IsArray())
	{
		auto list = CefListValue::Create();
		for (auto i = 0; i < valueV8->GetArrayLength(); i++)
		{
			auto index_value = valueV8->GetValue(i);

			// Prevent looping on circular structure
			if (!index_value->IsSame(valueV8))
			{
				list->SetValue(i, CefV8ValueToCefValue(index_value));
			}
		}

		value->SetList(std::move(list));
		return value;
	}

	if (valueV8->IsObject())
	{		
		auto dictionary = CefDictionaryValue::Create();
		if (std::vector<CefString> keys; valueV8->GetKeys(keys))
		{
			for (const auto& key : keys)
			{
				auto key_value = valueV8->GetValue(key);

				// Prevent looping on circular structure
				if (!key_value->IsSame(valueV8))
				{
					dictionary->SetValue(key, CefV8ValueToCefValue(key_value));
				}
			}
		}

		value->SetDictionary(std::move(dictionary));
		return value;
	}

	value->SetNull();
	return value;
}

struct DevToolsClient : CefClient
{
	IMPLEMENT_REFCOUNTING(DevToolsClient);
};

#define PMSG_SEND_IN "csp-msg-send"
#define PMSG_SEND_OUT "csp-msg-send-reply"
#define PMSG_RECEIVE_IN "csp-msg-receive"
#define PMSG_RECEIVE_OUT "csp-msg-receive-reply"
#define PMSG_FORM_DATA "csp-form-data"
#define PMSG_FILL_FORM "csp-fill-form"
#define PMSG_KILL "csp-msg-kill"

#define FLAGS cef_v8_propertyattribute_t(V8_PROPERTY_ATTRIBUTE_DONTENUM | V8_PROPERTY_ATTRIBUTE_DONTDELETE | V8_PROPERTY_ATTRIBUTE_READONLY)

struct ExchangeHandler : CefV8Handler
{
	ExchangeHandler(const CefRefPtr<CefBrowser>& browser, const CefRefPtr<CefV8Context>& context) : browser_(browser), context_(context)
	{
		if (!browser_) return;
		ac_ = CefV8Value::CreateObject(nullptr, nullptr);
		ac_->SetValue("sendAsync", CefV8Value::CreateFunction("sendAsync", this), FLAGS);
		ac_->SetValue("__formData", CefV8Value::CreateFunction("__formData", this), FLAGS);
		ac_->SetValue("onReceive", CefV8Value::CreateFunction("onReceive", this), FLAGS);
		ac_->SetValue("__listeners", CefV8Value::CreateObject(nullptr, nullptr), FLAGS);
		context->GetGlobal()->SetValue("AC", ac_, V8_PROPERTY_ATTRIBUTE_NONE);
	}

	bool Execute(const CefString& name,
		CefRefPtr<CefV8Value> object,
		const CefV8ValueList& arguments,
		CefRefPtr<CefV8Value>& retval,
		CefString& exception) override
	{
		if (name == "sendAsync")
		{
			if ((arguments.size() == 2 || arguments.size() == 3 && arguments[2]->IsFunction())
				&& arguments[0]->IsString())
			{
				auto message = CefProcessMessage::Create(PMSG_SEND_IN);
				const auto message_args = message->GetArgumentList();
				message_args->SetString(0, arguments[0]->GetStringValue());
				message_args->SetString(1, CefWriteJSON(CefV8ValueToCefValue(arguments[1]), JSON_WRITER_DEFAULT));
				if (arguments.size() == 3)
				{
					callbacks_[++last_callback_id_] = arguments[2];
					message_args->SetInt(2, last_callback_id_);
				}
				browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, std::move(message));
				retval = CefV8Value::CreateUndefined();
				return true;
			}
			exception = "wrong arguments (expected: <key: string>, <data: null|boolean|number|string|table>, [callback: function])";
		}
		else if (name == "__formData")
		{
			auto message = CefProcessMessage::Create(PMSG_FORM_DATA);
			message->GetArgumentList()->SetString(0, arguments[0]->GetStringValue());
			browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, std::move(message));
			retval = CefV8Value::CreateUndefined();
			return true;
		}
		else if (name == "onReceive")
		{
			if (arguments.size() == 2 && arguments[0]->IsString() && arguments[1]->IsFunction())
			{
				if (const auto receive = object->GetValue("__listeners"))
				{
					receive->SetValue(arguments[0]->GetStringValue(), arguments[1], FLAGS);
				}
				retval = CefV8Value::CreateUndefined();
				return true;
			}
			exception = "wrong arguments (expected: <key: string>, [callback: function])";
		}
		return false;
	}

	void TriggerCallback(int key, const CefString& value)
	{
		const auto f = callbacks_.find(key);
		if (f != callbacks_.end())
		{
			CefV8ValueList values;
			values.push_back(CefValueToCefV8Value(CefParseJSON(value, JSON_PARSER_ALLOW_TRAILING_COMMAS)));
			f->second->ExecuteFunction(ac_, values);
			callbacks_.erase(f);
		}
	}

private:
	IMPLEMENT_REFCOUNTING(ExchangeHandler);

	const CefRefPtr<CefBrowser> browser_;
	const CefRefPtr<CefV8Context> context_;
	CefRefPtr<CefV8Value> ac_;
	CefRefPtr<CefV8Value> receive_data_;
	std::unordered_map<int, CefRefPtr<CefV8Value>> callbacks_;
	int last_callback_id_{};
};

struct WebApp : CefApp, CefBrowserProcessHandler, CefRenderProcessHandler
{
	void OnScheduleMessagePumpWork(int64 delay_ms) override
	{
		// assert(delay_ms == 0);
	}

	void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override
	{
		registrar->AddCustomScheme("ac", CEF_SCHEME_OPTION_CSP_BYPASSING);
	}

	CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override
	{
		return this;
	}

	CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override
	{
		return this;
	}

	/*void OnBeforeChildProcessLaunch(CefRefPtr<CefCommandLine> command_line) override
	{
		std::string pid = std::to_string(GetCurrentProcessId());
		command_line->AppendSwitchWithValue("parent_pid", pid);
	}*/

	void OnBeforeCommandLineProcessing(const CefString& process_type, CefRefPtr<CefCommandLine> command_line) override
	{
		// https://github.com/chromiumembedded/cef/issues/3345
		command_line->AppendSwitchWithValue("disable-features", "CombineResponseBody,HardwareMediaKeyHandling,WebBluetooth");

		// command_line->AppendSwitchWithValue("touch-events", "1");
		// command_line->AppendSwitch("disable-gpu-shader-disk-cache");
		// command_line->AppendSwitch("enable-media-streams");
		// command_line->AppendSwitch("disable-gpu-vsync");
		// command_line->AppendSwitch("disable-accelerated-video-decode");

		// command_line->AppendSwitchWithValue("enable-media-stream", "true");
		// command_line->AppendSwitch("disable-web-security");
		// command_line->AppendSwitch("auto-accept-camera-and-microphone-capture");
		// command_line->AppendSwitch("disable-permissions-api");
				
		if (get_env_value(L"ACCSPWB_NO_PROXY_SERVER", true))
		{
			command_line->AppendSwitch("no-proxy-server");
		}

		if (get_env_value(L"ACCSPWB_FPS_COUNTER", false))
		{
			command_line->AppendSwitch("show-fps-counter");
		}

		if (get_env_value(L"ACCSPWB_AUTOPLAY", true))
		{
			command_line->AppendSwitchWithValue("autoplay-policy", "no-user-gesture-required");
		}

		// https://github.com/daktronics/cef-mixer/issues/10
		command_line->AppendSwitchWithValue("use-angle", "d3d11");
	}

	void OnContextInitialized() override {}

	void OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context) override
	{
		if (frame->IsMain())
		{
			exchange_handler_ = new ExchangeHandler(browser, context);
		}
	}

	void OnUncaughtException(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context, CefRefPtr<CefV8Exception> exception,
		CefRefPtr<CefV8StackTrace> stackTrace) override
	{
		log_message("OnUncaughtException: %s", exception->GetScriptResourceName().ToString().c_str());
	}

	void OnBrowserCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefDictionaryValue> extra_info) override { }
	void OnBrowserDestroyed(CefRefPtr<CefBrowser> browser) override { exchange_handler_ = nullptr; }

	bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefProcessId source_process,
		CefRefPtr<CefProcessMessage> message) override
	{
		if (message->GetName() == PMSG_KILL)
		{
			std::quick_exit(1);
		}
		if (message->GetName() == PMSG_RECEIVE_IN)
		{
			const auto args = message->GetArgumentList();
			if (args->GetSize() == 3)
			{
				std::string ret;
				if (const auto ctx = frame->GetV8Context())
				{
					ctx->Enter();
					if (const auto ac = ctx->GetGlobal()->GetValue("AC"))
					{
						if (const auto receive = ac->GetValue("__listeners"))
						{
							if (const auto rec = receive->GetValue(args->GetString(1)))
							{
								CefV8ValueList values;
								values.push_back(CefValueToCefV8Value(CefParseJSON(args->GetString(2), JSON_PARSER_ALLOW_TRAILING_COMMAS)));
								if (const auto v = rec->ExecuteFunction(nullptr, values))
								{
									ret = CefWriteJSON(CefV8ValueToCefValue(v), JSON_WRITER_DEFAULT);
								}
							}
						}
					}
					ctx->Exit();
				}

				auto reply = CefProcessMessage::Create(PMSG_RECEIVE_OUT);
				const auto reply_args = reply->GetArgumentList();
				reply_args->SetString(0, args->GetString(0));
				reply_args->SetString(1, ret);
				frame->SendProcessMessage(PID_BROWSER, std::move(reply));
			}
			return true;
		}
		if (message->GetName() == PMSG_SEND_OUT)
		{
			const auto args = message->GetArgumentList();
			if (args->GetSize() == 2 && exchange_handler_)
			{
				if (const auto ctx = frame->GetV8Context())
				{
					ctx->Enter();
					exchange_handler_->TriggerCallback(args->GetInt(0), args->GetString(1));
					ctx->Exit();
				}
			}
			return true;
		}
		if (message->GetName() == PMSG_FILL_FORM)
		{
			const auto args = message->GetArgumentList();
			log_message("got form filling msg");
			if (auto s = args->GetSize(); s > 1 && s % 2 == 1)
			{
				struct : CefDOMVisitor
				{
					CefString tag_name_form{"FORM"};
					CefString tag_name_input{"INPUT"};
					CefString tag_name_svg{"svg"};
					CefString tag_name_a{"A"};
					CefString attr_name_action{"action"};
					CefString attr_name_name{"name"};
					CefString attr_name_value{"value"};
					std::string target_action;
					std::unordered_map<std::string, std::string> values;
					uint32_t seen_nodes = 0U;

					bool action_matches(const std::string& form_action)
					{
						return target_action.ends_with(form_action);
					}

					void iterate_children(const CefRefPtr<CefDOMNode>& node)
					{
						for (auto child = node->GetFirstChild(); child; child = child->GetNextSibling())
						{
							const auto tag_name = child->GetElementTagName();
							const auto s = tag_name.size();
							
							if (s == 0ULL) continue;
							if (++seen_nodes > 4000) return;
							if (s == 4 && tag_name == tag_name_form)
							{
								if (action_matches(child->GetElementAttribute(attr_name_action).ToString()))
								{
									iterate_children(child);
								}
							}
							else if (s == 5 && tag_name == tag_name_input)
							{
								auto name = child->GetElementAttribute(attr_name_name).ToString();									
								if (auto found = values.find(name); found != values.end() && child->GetElementAttribute(attr_name_value).empty())
								{
									child->SetElementAttribute(attr_name_value, found->second);
								}
							}
							else if (!(s == 1 && tag_name == tag_name_a || s == 3 && tag_name == tag_name_svg))
							{
								iterate_children(child);
							}
						}
					}

					void Visit(CefRefPtr<CefDOMDocument> document) override
					{
						iterate_children(document->GetBody());
					}

					STACK_ENTITY
				} visitor;
				
				visitor.target_action = args->GetString(0).ToString();
				for (auto i = 1U; i < s; i += 2)
				{
					visitor.values[args->GetString(i).ToString()] = args->GetString(i + 1).ToString();
				}
				auto n = time_now_ms();
				frame->VisitDOM(&visitor);
				auto e = time_now_ms() - n;
				log_message("Time taken to visit DOM: %.3f ms, seen: %d", e, visitor.seen_nodes);
			}
			return true;
		}
		return false;
	}

private:
	IMPLEMENT_REFCOUNTING(WebApp);
	CefRefPtr<ExchangeHandler> exchange_handler_;
};

struct FrameBuffer
{
	FrameBuffer(const d3d11::Device& device) : device_(device), dirty_(false) {}
	uint32_t width() const { return shared_buffer_ ? shared_buffer_->width() : 0U; }
	uint32_t height() const { return shared_buffer_ ? shared_buffer_->height() : 0U; }

	void on_paint(const void* buffer, uint32_t width, uint32_t height)
	{
		const auto stride = width * 4;
		const size_t cb = stride * height;

		if (!shared_buffer_ || shared_buffer_->width() != width || shared_buffer_->height() != height)
		{
			shared_buffer_ = device_.create_texture(width, height, DXGI_FORMAT_B8G8R8A8_UNORM, nullptr, 0);
			sw_buffer_ = std::shared_ptr<uint8_t>((uint8_t*)malloc(cb), free);
		}
		if (sw_buffer_ && buffer)
		{
			memcpy(sw_buffer_.get(), buffer, cb);
		}
		dirty_ = true;
	}

	void on_gpu_paint(void* shared_handle)
	{
		std::lock_guard guard(lock_);
		if (shared_buffer_ && shared_handle != shared_buffer_->share_handle())
		{
			shared_buffer_.reset();
		}
		if (!shared_buffer_)
		{
			shared_buffer_ = device_.open_shared_texture_nt(shared_handle);
			if (!shared_buffer_)
			{
				std::cerr << "Failed to open shared texture" << std::endl;
				std::quick_exit(20);
			}
		}
		dirty_ = true;
	}

	d3d11::Texture2D* swap(const d3d11::Context& ctx)
	{
		std::lock_guard guard(lock_);
		if (sw_buffer_ && shared_buffer_ && dirty_)
		{
			shared_buffer_->copy_from(ctx, sw_buffer_.get(), shared_buffer_->width() * 4, shared_buffer_->height());
		}
		dirty_ = false;
		return shared_buffer_.get();
	}

private:
	std::mutex lock_;
	std::atomic_bool abort_;
	std::shared_ptr<d3d11::Texture2D> shared_buffer_;
	const d3d11::Device& device_;
	std::shared_ptr<uint8_t> sw_buffer_;
	bool dirty_;
};

// A simple layer that will render out PET_POPUP for a corresponding view.
struct PopupLayer : Layer
{
	PopupLayer(const d3d11::Device& device, std::shared_ptr<FrameBuffer> buffer)
		: Layer(device, true), frame_buffer_(std::move(buffer)) {}

	void render(const d3d11::Context& ctx) override
	{
		if (frame_buffer_)
		{
			render_texture(ctx, frame_buffer_->swap(ctx));
		}
	}

private:
	const std::shared_ptr<FrameBuffer> frame_buffer_;
};

struct HtmlSourceWriter : CefStringVisitor
{
	HtmlSourceWriter(const std::string& filename)
	{
		fout_ = std::make_shared<std::ofstream>(filename);
	}

	void Visit(const CefString& string) override
	{
		if (fout_ && fout_->is_open())
		{
			const auto utf8 = string.ToString();
			fout_->write(utf8.c_str(), utf8.size());
		}
	}

private:
	IMPLEMENT_REFCOUNTING(HtmlSourceWriter);
	std::shared_ptr<std::ofstream> fout_;
};

static std::mutex _alive_mutex;
static std::unordered_map<int, WebView*> _alive_instances; 

struct WebView : CefClient, CefRequestHandler, CefResourceRequestHandler, CefRenderHandler, CefDisplayHandler, CefDialogHandler,
	CefDownloadHandler, CefLifeSpanHandler, CefLoadHandler, CefJSDialogHandler, CefContextMenuHandler, CefSchemeHandlerFactory,
	CefFindHandler, CefAudioHandler
{
private:
	std::shared_ptr<accsp_mapped_typed<accsp_wb_entry>> mmf;
	const d3d11::Device& device_;
	int width_;
	int height_;
	int key_;
	bool passthrough_mode_;
	bool redirect_audio_;
	bool has_full_access_;
	std::shared_ptr<FrameBuffer> view_buffer_;
	std::shared_ptr<FrameBuffer> popup_buffer_;

public:
	WebView(std::shared_ptr<accsp_mapped_typed<accsp_wb_entry>> mmf_, const d3d11::Device& device, bool passthrough_mode, bool redirect_audio, int key, bool has_full_access)
		: mmf(std::move(mmf_)), device_(device), width_(mmf->entry->width), height_(mmf->entry->height), key_(key),
		passthrough_mode_(passthrough_mode), redirect_audio_(redirect_audio), has_full_access_(has_full_access),
		view_buffer_(passthrough_mode ? nullptr : std::make_shared<FrameBuffer>(device)),
		popup_buffer_(passthrough_mode ? nullptr : std::make_shared<FrameBuffer>(device))
	{
		std::string delayed;
		auto delayed_count = 0U;
		if (iterate_commands([&](command_be k, const utils::str_view& v)
		{
			if (!configure_control(k, v))
			{
				auto d = delayed.size();
				delayed.resize(delayed.size() + 3 + v.size());
				*(command_be*)&delayed[d] = k;
				*(uint16_t*)&delayed[d + 1] = uint16_t(v.size());
				memcpy(&delayed[d + 3], v.data(), v.size());
				++delayed_count;
			}
		}))
		{
			memcpy(mmf->entry->commands, delayed.data(), delayed.size());
			mmf->entry->commands_set = delayed_count;
		}

		std::unique_lock lock(_alive_mutex);
		_alive_instances[key] = this;
	}

	~WebView() override
	{
		log_message("~WebView(%p)", this);
		{
			std::unique_lock lock(_alive_mutex);
			_alive_instances[key_] = this;
		}
		close();
	}

	template<typename Callback>
	bool iterate_commands(Callback&& callback)
	{
		auto entry = mmf->entry;
		if (entry->commands_set == 0) return false;
		auto p = entry->commands;
		for (auto _ = entry->commands_set; _ > 0; --_)
		{
			auto k = p[0];
			auto s = *(uint16_t*)&p[1];
			p += 3;
			callback(command_be(k), utils::str_view(p, 0, s));
			p += s;
		}
		__faststorefence();
		return true;
	}

	/*void OnRequestGeolocationPermission(CefRefPtr<CefBrowser> browser, const CefString& requesting_url, int request_id,
		CefRefPtr<CefGeolocationCallback> callback) override
	{
		callback->Continue(true);
	}*/

	bool GetAuthCredentials(CefRefPtr<CefBrowser> browser, const CefString& origin_url, bool proxy, const CefString& host, int port, const CefString& realm, 
		const CefString& scheme, CefRefPtr<CefAuthCallback> callback) override
	{
		lson_builder b;
		b.add("host", host);
		b.add("port", port);
		b.add("realm", realm);
		b.add("scheme", scheme);
		b.add("proxy", proxy);
		b.add("originURL", origin_url);
		b.add("replyID", await_reply("AuthCredentials", [callback = callback](const utils::str_view& v)
		{
			if (v.empty())
			{
				callback->Cancel();
			}
			else
			{
				const auto kv = v.pair('\1');
				callback->Continue(kv.first, kv.second);
			}
		}));
		set_response(command_fe::auth_credentials, b.finalize());
		return true;
	}

	CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }
	CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
	CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
	CefRefPtr<CefDialogHandler> GetDialogHandler() override { return this; }
	CefRefPtr<CefDownloadHandler> GetDownloadHandler() override { return this; }
	CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
	CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
	CefRefPtr<CefJSDialogHandler> GetJSDialogHandler() override { return this; }
	CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }

	CefRefPtr<CefAudioHandler> GetAudioHandler() override
	{
		if (redirect_audio_) return this;
		return nullptr;
	}

	std::unique_ptr<accsp_mapped> audio_buffer;

	struct stream_data : noncopyable
	{
		uint32_t frequency;
		uint32_t channels;
		uint32_t format;
		uint32_t buffer_size;
		uint32_t target_gap;
		uint32_t _pad;
		int64_t written_bytes;
	};

	bool GetAudioParameters(CefRefPtr<CefBrowser> browser, CefAudioParameters& params) override
	{
		params.channel_layout = CEF_CHANNEL_LAYOUT_STEREO;
		params.sample_rate = 48000;
		params.frames_per_buffer = 1920;
		return true;
	}

	#define MMF_PREFIX_SIZE (16 * sizeof(int))
	#define MMF_ITEMS_COUNT (1920 * 32)
	#define MMF_SIZE (MMF_PREFIX_SIZE + MMF_ITEMS_COUNT * sizeof(float))

	int audio_frame_next_pos_ = 0;
	bool audio_frame_first_ = true;

	void OnAudioStreamStarted(CefRefPtr<CefBrowser> browser, const CefAudioParameters& params, int channels) override
	{
		log_message("Audio stream started: %d, %d", params.channel_layout, params.sample_rate);
		assert(params.channel_layout == CEF_CHANNEL_LAYOUT_STEREO);
		assert(params.sample_rate == 48000);
		if (!audio_buffer)
		{
			audio_buffer = std::make_unique<accsp_mapped>(named_prefix + L"!", MMF_SIZE, false);
			auto& data = *(stream_data*)audio_buffer->entry;
			data.frequency = params.sample_rate;
			data.channels = 2;
			data.format = 5;
			data.buffer_size = 48000 / 25;
			data.target_gap = sizeof(float) * 48000 / 25;
			data._pad = 0U;
		}
		base_flags |= 256;
		mmf->entry->be_flags |= 256;
		set_response(command_fe::audio, "1");
	}

	void OnAudioStreamPacket(CefRefPtr<CefBrowser> browser, const float** data, int frames, int64 pts) override
	{
		if (!audio_buffer || frames == 0) return;
		
		auto ptr = (byte*)audio_buffer->entry;
		if ((base_flags & 16ULL) != 0)
		{
			audio_frame_first_ = true;
			mmf->entry->audio_peak = 0;
			*(int64_t*)&ptr[24] = 0;
			return;
		}

		auto peak = 0.f;
		if (audio_frame_first_)
		{
			audio_frame_next_pos_ = 0;
		}

		const auto left = MMF_ITEMS_COUNT - audio_frame_next_pos_;
		const auto num_samples = frames * 2;
		const auto block1 = std::min(left, num_samples);
		auto dst = (float*)&ptr[MMF_PREFIX_SIZE + audio_frame_next_pos_ * sizeof(float)];
		for (auto i = 0; i < block1; i++)
		{
			auto f = (i % 2 ? data[1] : data[0])[i / 2];					
			if (auto a = std::abs(f); a > peak) peak = a;
			*dst = f;
			++dst;
		}

		if (num_samples >= left)
		{
			const auto block2 = num_samples - left;
			if (block2 > 0)
			{
				dst = (float*)&ptr[MMF_PREFIX_SIZE];
				for (auto i = 0; i < block2; i++)
				{
					auto f = (i % 2 ? data[1] : data[0])[i / 2];					
					if (auto a = std::abs(f); a > peak) peak = a;
					*dst = f;
					++dst;
				}
			}
			audio_frame_next_pos_ = block2;
		}
		else
		{
			audio_frame_next_pos_ += num_samples;
		}

		mmf->entry->audio_peak = uint8_t(std::min(peak, 1.f) * 255.f);
		MemoryBarrier();
		if (audio_frame_first_)
		{
			*(int64_t*)&ptr[24] = num_samples * sizeof(float);
			audio_frame_first_ = false;
		}
		else
		{
			*(int64_t*)&ptr[24] += num_samples * sizeof(float);
		}
	}

	void OnAudioStreamError(CefRefPtr<CefBrowser> browser, const CefString& message) override
	{
		std::cout << "Audio stream error: " << message.ToString() << std::endl;
		log_message("Audio stream error: %s", message.ToString().c_str());
		base_flags &= ~256ULL;
		mmf->entry->be_flags &= ~256ULL;
		mmf->entry->audio_peak = 0;
		audio_frame_first_ = true;
		audio_frame_next_pos_ = 0;
		set_response(command_fe::audio, "0");
	}

	void OnAudioStreamStopped(CefRefPtr<CefBrowser> browser) override
	{
		log_message("Audio stream stopped");
		base_flags &= ~256ULL;
		mmf->entry->be_flags &= ~256ULL;
		mmf->entry->audio_peak = 0;
		audio_frame_first_ = true;
		audio_frame_next_pos_ = 0;
		set_response(command_fe::audio, "0");
	}

	struct async_handler : CefResourceHandler
	{
		static constexpr int INVALID_STATUS = INT32_MAX - 1;

		std::mutex response_mutex;
		std::string mime_type;
		std::string headers;
		std::string body;
		CefRefPtr<CefCallback> response_callback;
		int status_code = INVALID_STATUS;
		int pos = 0;

		void set_response(int status_code_, std::string mime_type_, std::string headers_, std::string body_)
		{
			{
				std::unique_lock lock(response_mutex);
				status_code = status_code_ == INVALID_STATUS ? 0 : status_code_;
				mime_type = std::move(mime_type_);
				headers = std::move(headers_);
				body = std::move(body_);
			}
			if (response_callback)
			{
				response_callback->Continue();
			}
		}

		bool Open(CefRefPtr<CefRequest> request, bool& handle_request, CefRefPtr<CefCallback> callback) override
		{
			std::unique_lock lock(response_mutex);
			if (status_code != INVALID_STATUS)
			{
				handle_request = true;
			}
			else
			{
				response_callback = std::move(callback);
				handle_request = false;
			}
			return true;
		}

		void Cancel() override { }

		void GetResponseHeaders(CefRefPtr<CefResponse> r, int64& response_length, CefString& redirect_url) override
		{
			r->SetStatus(status_code);
			r->SetMimeType(mime_type);
			r->SetCharset("utf-8");
			for (const auto& p : utils::str_view::from_str(headers).pairs('\2'))
			{
				if (p.first.empty())
				{
					redirect_url = p.second;
				}
				else
				{
					r->SetHeaderByName(p.first, p.second, true);
				}
			}
			response_length = int64_t(body.size());
		}

		bool Skip(int64 bytes_to_skip, int64& bytes_skipped, CefRefPtr<CefResourceSkipCallback> callback) override
		{
			pos += bytes_to_skip;
			bytes_skipped = bytes_to_skip;
			return true;
		}

		bool Read(void* data_out, int bytes_to_read, int& bytes_read, CefRefPtr<CefResourceReadCallback> callback) override
		{
			auto left = int(body.size()) - pos;
			if (left <= 0)
			{
				bytes_read = 0;
				return false;
			}
			if (left < bytes_to_read) bytes_to_read = left;
			memcpy(data_out, &body[pos], bytes_to_read);
			pos += bytes_to_read;
			bytes_read = bytes_to_read;
			return true;
		}

	private:
		IMPLEMENT_REFCOUNTING(async_handler);
	};

	CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, const CefString& scheme_name,
		CefRefPtr<CefRequest> request) override
	{
		CefRefPtr ret(new async_handler());
		std::vector<std::string> data;
		data.push_back(await_reply("ac://", [ret = ret](const utils::str_view& reply)
		{
			if (auto pieces = reply.split('\1', false, false, 4); pieces.size() == 4)
			{
				ret->set_response(pieces[0].as(0), pieces[1].str(), pieces[2].str(), pieces[3].str());
			}
			else
			{
				ret->set_response(500, "text/plain", "", "Damaged exchange");
			}
		}));
		data.push_back(request->GetURL().ToString());
		data.push_back(request->GetMethod().ToString());
		{
			lson_builder headers;
			CefRequest::HeaderMap map;
			request->GetHeaderMap(map);
			for (auto& p : map)
			{
				headers.add(p.first.ToString().c_str(), p.second);
			}
			data.push_back(headers.finalize());
		}
		std::string s;
		if (data[2] != "GET")
		{
			auto p = request->GetPostData();
			assert(p->GetElementCount() < 2);
			if (p->GetElementCount() == 1)
			{
				CefPostData::ElementVector elements;
				p->GetElements(elements);
				s.resize(elements[0]->GetBytesCount());
				elements[0]->GetBytes(s.size(), s.data());
			}
		}
		data.push_back(std::move(s));
		set_response(command_fe::csp_scheme_request, std::move(data));
		return ret;
	}

	struct BasicTask : CefTask
	{
		std::function<void()> fn;
		BasicTask(std::function<void()> fn) : fn(std::move(fn)) { }
		void Execute() override { fn(); }
		IMPLEMENT_REFCOUNTING(BasicTask);
	};

	void attach(Composition* composition)
	{
		if (!passthrough_mode_ && composition)
		{
			popup_layer_ = composition->add_layer(std::make_unique<PopupLayer>(device_, popup_buffer_));
		}
		else
		{
			popup_layer_ = nullptr;
		}
	}

	void close()
	{
		log_message("WebView::close(%p): ref_count=%d, browser=%p", this, (*(base::AtomicRefCount*)&ref_count_).SubtleRefCountForDebug(), browser_ptr_.load());
		if (const auto browser = browser_ptr_.exchange(nullptr))
		{
			browser->GetHost()->CloseBrowser(true);
			auto release = browser->Release();
			log_message("browser->Release(): %d", release); 
		}
	}

	enum class command_fe : char
	{
		large_command = '\2',
		
		load_start = '/',
		load_end = '0',
		open_url = '1',
		popup = '2',
		jsdialog_dialog = '3',
		download = '4',
		context_menu = '5',
		load_failed = '6',
		found_result = '7',
		file_dialog = '8',
		auth_credentials = '9',
		form_data = ':',
		custom_scheme_browse = ';',

		reply = '\1',
		data_from_script = 'R',
		url_monitor = 'm',
		csp_scheme_request = 'S',
		download_update = 'r',
		close = 'x',

		favicon = 'I',
		url = 'U',
		title = 'T',
		status = '?',
		tooltip = 'O',
		audio = 'A',
		virtual_keyboard_request = 'v',
	};

	static bool is_command_overriding(command_fe k)
	{
		return k == command_fe::load_failed
			|| k == command_fe::favicon
			|| k == command_fe::url
			|| k == command_fe::title
			|| k == command_fe::status
			|| k == command_fe::tooltip
			|| k == command_fe::audio;
	}

	enum class command_be : char
	{
		large_command = '\2',

		navigate = 'N',
		set_option = 'i', \
		filter_resource_urls = 'f',
		set_headers = 'h',
		inject_js = 'j',
		inject_css = 's',

		zoom = 'z',
		reload = 'R',
		stop = 'S',
		lifespan = 'U',
		download = 'W',
		command = 'C',
		input = 'I',
		key_down = '>',
		key_up = '<',
		find = 'd',
		mute = 'M',
		capture_lost = 'A',
		execute = 'E',
		dev_tools_message = 'w',
		send = 'e',
		scroll = 'l',

		reply = '\1',
		html = 'H',
		text = 'T',
		history = 'Y',
		write_cookies = 'o',
		read_cookies = 'c',
		ssl = 'L',
		download_image = 'n',
		control_download = 'r',
		fill_form = 'F',
		awake = 'K',
		color_scheme = 'm',
	};

	bool RunContextMenu(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefContextMenuParams> params, CefRefPtr<CefMenuModel> model,
		CefRefPtr<CefRunContextMenuCallback> callback) override
	{
		lson_builder b;
		b.add("originURL", params->GetFrameUrl());
		b.add_opt("sourceURL", params->GetSourceUrl());
		b.add_opt("linkURL", params->GetLinkUrl());
		b.add_opt("unfilteredLinkURL", params->GetUnfilteredLinkUrl());
		b.add("x", params->GetXCoord());
		b.add("y", params->GetYCoord());
		b.add_opt("selectedText", params->GetSelectionText());
		b.add("editable", params->IsEditable());
		b.add_opt("titleText", params->GetTitleText());
		set_response(command_fe::context_menu, b.finalize());
		return true;
	}

	/*static bool is_standard_scheme(const std::string& s)
	{
		static auto& v = *[]
		{
			auto r = new std::unordered_set<std::string>;
			r->insert("http");
			r->insert("https");
			r->insert("ftp");
			r->insert("file");
			r->insert("data");
			r->insert("blob");
			r->insert("about");
			r->insert("chrome");
			r->insert("chrome-extension");
			r->insert("javascript");
			r->insert("ac");
			return r;
		}();
		return v.contains(s);
	}*/

	bool last_browse_nonget{};
	// bool has_post_processor{};
	// std::string post_processor;
	// std::mutex post_processor_mutex;

	bool track_form_data{};
	std::atomic_uint8_t track_form_data_state{};
	std::mutex track_form_mutex;
	std::string form_request_url;
	std::string form_original_url;
	std::string form_data_ready;

	void OnProtocolExecution(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> request, bool& allow_os_execution) override
	{
		log_message("OnProtocolExecution(%s)", request->GetURL().ToString().c_str());
		allow_os_execution = false;
	}

	// void OnTextSelectionChanged(CefRefPtr<CefBrowser> browser, const CefString& selected_text, const CefRange& selected_range) override;

	void OnVirtualKeyboardRequested(CefRefPtr<CefBrowser> browser, TextInputMode input_mode) override
	{
		log_message("OnVirtualKeyboardRequested(%d)", int(input_mode));
		std::string ret;
		switch (input_mode)
		{
			case CEF_TEXT_INPUT_MODE_NONE: ret = ""; break;
			case CEF_TEXT_INPUT_MODE_DEFAULT: ret = "default"; break;
			case CEF_TEXT_INPUT_MODE_TEXT: ret = "text"; break;
			case CEF_TEXT_INPUT_MODE_TEL: ret = "tel"; break;
			case CEF_TEXT_INPUT_MODE_URL: ret = "url"; break;
			case CEF_TEXT_INPUT_MODE_EMAIL: ret = "email"; break;
			case CEF_TEXT_INPUT_MODE_NUMERIC: ret = "numeric"; break;
			case CEF_TEXT_INPUT_MODE_DECIMAL: ret = "decimal"; break;
			case CEF_TEXT_INPUT_MODE_SEARCH: ret = "search"; break;
			default: return;;
		}
		set_response(command_fe::virtual_keyboard_request, std::move(ret));
	}
	
	bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> request,
		bool user_gesture, bool is_redirect) override
	{
		if (!redirect_nonstandard_schemes_filter._Empty())
		{
			std::unique_lock lock(redirect_nonstandard_schemes_mutex);
			if (test_regex(request->GetURL().ToString(), redirect_nonstandard_schemes_filter))
			{
				if (frame->IsMain())
				{
					lson_builder b;
					b["userGesture"] = user_gesture;
					b["redirect"] = is_redirect;
					b["originURL"] = frame->GetURL();
					b["targetURL"] = request->GetURL();
					set_response(command_fe::custom_scheme_browse, b.finalize());
				}
				return true;
			}
		}
		if (frame->IsMain())
		{
			last_browse_nonget = request->GetMethod() != "GET";
			base_flags = last_browse_nonget ? (base_flags | 128) : (base_flags & ~128);
			update_url(request->GetURL());

			/*if (last_browse_nonget && has_post_processor)
			{
				std::unique_lock lock(post_processor_mutex);
				frame->ExecuteJavaScript(post_processor, "", 0);
			}*/

			if (track_form_data && request->GetMethod() == "POST")
			{
				frame->ExecuteJavaScript("AC.__formData([].map.call(document.querySelectorAll('form[method=post] input"
					":not([type=hidden]):not([type=submit]):not([type=button]):not([type=checkbox]):not([type=color]):not([type=date])"
					":not([type=datetime-local]):not([type=file]):not([type=image]):not([type=month]):not([type=radio]):not([type=range])"
					":not([type=reset]):not([type=time]):not([type=week])"
					"'), i => [i.form.action, i.type, i.name == '' ? '\\n' : i.name, i.value == '' ? '\\n' : i.value])"
					".filter(x => /^[^\\n\\r]{1,400}$/.test(x)).map(x => x.join('\\r')).join('\\n'))", "", 0);
				std::unique_lock lock(track_form_mutex);
				track_form_data_state = 1;
				form_original_url = frame->GetURL();
				form_request_url = request->GetURL();
				form_data_ready.clear();
			}
		}
		return false;
	}

	void process_form_data(const CefString& data)
	{
		std::string url, original_url;
		{
			std::unique_lock lock(track_form_mutex);
			if (form_original_url.empty()) return;
			url.swap(form_request_url);
			original_url.swap(form_original_url);
		}

		log_message("Got form data: %s", url.c_str());
		auto str = data.ToString();
		auto pieces = utils::str_view::from_str(str).split('\n', false, false);
		lson_builder b;
		for (auto p : pieces)
		{
			auto v = p.split('\r', false, false);
			if (v.size() == 4 && v[0] == url)
			{
				b.add(v[2].str().c_str(), lson_builder{}.add("type", v[1]).add("value", v[3]));
				log_message("Form input: URL=%s, type=%s, name=%s, value=%s", v[0].str().c_str(), v[1].str().c_str(), v[2].str().c_str(), v[3].str().c_str());
			}
		}
		if (!b.empty())
		{
			auto finalized = lson_builder{}.add("actionURL", url).add("originURL", original_url).add("form", b).finalize();

			std::unique_lock lock(track_form_mutex);		
			if (track_form_data_state == 2)
			{
				set_response(command_fe::form_data, std::move(finalized));
			}
			else
			{
				form_data_ready = std::move(finalized);
			}
		}
	}

	bool OnJSDialog(CefRefPtr<CefBrowser> browser, const CefString& origin_url, JSDialogType dialog_type, const CefString& message_text,
		const CefString& default_prompt_text, CefRefPtr<CefJSDialogCallback> callback, bool& suppress_message) override
	{
		// suppress_message = true;
		lson_builder b;
		b.add("type", dialog_type == JSDIALOGTYPE_ALERT ? "alert" : dialog_type == JSDIALOGTYPE_CONFIRM ? "confirm" : "prompt");
		b.add("message", message_text);
		b.add("originURL", origin_url);
		b.add("replyID", await_reply("JS dialog", [callback = callback](const utils::str_view& v)
		{
			const auto kv = v.pair('\1');
			callback->Continue(kv.first == "1", kv.second);
		}));
		if (dialog_type == JSDIALOGTYPE_PROMPT)
		{
			b.add("defaultPrompt", default_prompt_text);
		}
		set_response(command_fe::jsdialog_dialog, b.finalize());
		return true;
	}

	bool OnBeforeUnloadDialog(CefRefPtr<CefBrowser> browser, const CefString& message_text, bool is_reload,
		CefRefPtr<CefJSDialogCallback> callback) override
	{
		lson_builder b;
		b.add("type", "beforeUnload");
		b.add("message", message_text);
		b.add("reload", is_reload);
		b.add("replyID", await_reply("BeforeUnload dialog", [callback = callback](const utils::str_view& v)
		{
			const auto kv = v.pair('\1');
			callback->Continue(kv.first == "1", kv.second);
		}));
		set_response(command_fe::jsdialog_dialog, b.finalize());
		return true;
	}

	std::unordered_map<uint64_t, std::function<void(const utils::str_view& data)>> awaiting_reply;
	uint64_t last_reply_id{};

	std::string await_reply(const char* hint, std::function<void(const utils::str_view& data)> fn)
	{
		if (!fn) return {};
		const auto id = ++last_reply_id;
		awaiting_reply[id] = std::move(fn);
		#if _DEBUG
		/*std::thread([this, hint, id = id]
		{
			Sleep(1000);
			if (awaiting_reply.contains(id))
			{
				std::cerr << "Reply did not arrive in time: " << hint << "; " << id << std::endl;
			}
		}).detach();*/
		#endif
		return std::to_string(id);
	}

	bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefProcessId source_process,
		CefRefPtr<CefProcessMessage> message) override
	{
		if (message->GetName() == PMSG_SEND_IN)
		{
			std::string ret;
			const auto args = message->GetArgumentList();
			if (args->GetSize() == 3)
			{
				ret += await_reply("AC.send", [k = args->GetInt(2), f = frame](const utils::str_view& data)
				{
					auto reply = CefProcessMessage::Create(PMSG_SEND_OUT);
					const auto reply_args = reply->GetArgumentList();
					reply_args->SetInt(0, k);
					reply_args->SetString(1, data);
					f->SendProcessMessage(PID_RENDERER, std::move(reply));
				});
			}
			ret.push_back('\1');
			ret += args->GetString(0);
			ret.push_back('\1');
			ret += args->GetString(1);
			set_response(command_fe::data_from_script, std::move(ret));
			return true;
		}
		if (message->GetName() == PMSG_RECEIVE_OUT)
		{
			set_reply(message->GetArgumentList()->GetString(0), message->GetArgumentList()->GetString(1));
			return true;
		}
		if (message->GetName() == PMSG_FORM_DATA)
		{
			process_form_data(message->GetArgumentList()->GetString(0));
			return true;
		}
		return false;
	}

	struct paint_data
	{
		void* reshared{};
		uint64_t current{};
		std::vector<void*> kept_alive;

		void clean()
		{
			for (auto i : kept_alive)
			{
				CloseHandle(i);
			}
			kept_alive.clear();
		}

		void reset()
		{
			clean();
			CloseHandle(reshared);
			reshared = nullptr;
			current = 0ULL;
		}

		void update(const d3d11::Device& device, void* received, const std::wstring& prefix, uint64_t& next)
		{
			if (reshared)
			{
				if (kept_alive.size() > 4)
				{
					CloseHandle(kept_alive[0]);
					kept_alive.erase(kept_alive.begin());
				}
				kept_alive.push_back(reshared);
				reshared = nullptr;
			}


			if (++next > 1024) next = 1;
			current = next;
			device.recreate_shared_texture_nt((prefix + L"." + std::to_wstring(current)).c_str(), received, reshared);
		}
	};

	paint_data pd_main, pd_popup;
	std::wstring named_prefix;
	uint64_t prefix_index{};
	std::array<float, 4> popup_area{};
	bool popup_active{};
	bool limited{};

	void OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type, const RectList& dirty_rects, const void* buffer, int width, int height) override
	{
		if (!passthrough_mode_)
		{
			(type == PET_VIEW ? view_buffer_ : popup_buffer_)->on_paint(buffer, width, height);
		}
		else
		{
			std::cerr << "Can not use OnPaint with passthrough mode" << std::endl;
		}
	}

	void OnAcceleratedPaint2(CefRefPtr<CefBrowser> browser, PaintElementType type, const RectList& dirty_rects, void* shared_handle, bool new_texture) override
	{
		log_message("OAP2: type=%d, handle=%p, new=%d", int(type), shared_handle, new_texture);
		if (!passthrough_mode_)
		{
			(type == PET_VIEW ? view_buffer_ : popup_buffer_)->on_gpu_paint(shared_handle);
		}
		else if (!named_prefix.empty() && new_texture)
		{
			(type == PET_VIEW ? pd_main : pd_popup).update(device_, shared_handle, named_prefix, prefix_index);
		}
	}

	void OnAcceleratedPaint(CefRefPtr<CefBrowser> browser, PaintElementType type, const RectList& dirty_rects, void* shared_handle) override
	{
		if (!passthrough_mode_)
		{
			(type == PET_VIEW ? view_buffer_ : popup_buffer_)->on_gpu_paint(shared_handle);
		}
		else if (!named_prefix.empty())
		{
			(type == PET_VIEW ? pd_main : pd_popup).update(device_, shared_handle, named_prefix, prefix_index);
		}
	}

	void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) override
	{
		rect.Set(0, 0, last_hidden ? width_ + 1 : width_, height_);
		if (rect.width < 4) rect.width = 4;
		if (rect.height < 4) rect.height = 4;
	}

	void OnAfterCreated(CefRefPtr<CefBrowser> browser) override
	{
		if (!CefCurrentlyOn(TID_UI))
		{
			assert(0);
			return;
		}
		
		if (CefBrowser* cur{}; browser_ptr_.compare_exchange_strong(cur, browser.get()))
		{
			browser->AddRef();
			sync();
			if (was_resized)
			{
				browser->GetHost()->WasResized();
			}
			if (redirect_audio_)
			{
				browser->GetHost()->SetAudioMuted(true);
			}

			own_zoom_phase = UINT32_MAX;
		}
	}

	void OnPopupShow(CefRefPtr<CefBrowser> browser, bool show) override
	{
		if (passthrough_mode_)
		{
			popup_active = show;
		}
		else if (popup_layer_)
		{
			popup_layer_->move(0.f, 0.f, 0.f, 0.f);
		}
	}

	void OnPopupSize(CefRefPtr<CefBrowser> browser, const CefRect& rect) override
	{
		if (passthrough_mode_)
		{
			popup_area[0] = float(rect.x) / float(width_);
			popup_area[1] = float(rect.y) / float(height_);
			popup_area[2] = float(rect.x + rect.width) / float(width_);
			popup_area[3] = float(rect.y + rect.height) / float(height_);
		}
		else if (popup_layer_)
		{
			if (const auto composition = popup_layer_->composition())
			{
				const auto outer_width = composition->width();
				const auto outer_height = composition->height();
				if (outer_width > 0 && outer_height > 0)
				{
					popup_layer_->move(float(rect.x) / float(outer_width), float(rect.y) / float(outer_height),
						float(rect.width) / float(outer_width), float(rect.height) / float(outer_height));
				}
			}
		}
	}

	bool OnBeforePopup(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, const CefString& target_url,
		const CefString& target_frame_name, cef_window_open_disposition_t target_disposition, bool user_gesture,
		const CefPopupFeatures& popup_features, CefWindowInfo& windowInfo, CefRefPtr<CefClient>& client,
		CefBrowserSettings& settings, CefRefPtr<CefDictionaryValue>& extra_info, bool* no_javascript_access) override
	{
		lson_builder b;
		b["userGesture"] = user_gesture;
		b["originURL"] = frame->GetURL();
		b["targetURL"] = target_url;
		b["targetFrameName"] = target_frame_name;
		b["targetDisposition"] = encode_wodisp(target_disposition);
		{
			lson_builder f;
			if (popup_features.widthSet) f["width"] = popup_features.width;
			if (popup_features.heightSet) f["height"] = popup_features.height;
			if (popup_features.xSet) f["x"] = popup_features.x;
			if (popup_features.ySet) f["y"] = popup_features.y;
			f["menuBarVisible"] = popup_features.menuBarVisible;
			f["statusBarVisible"] = popup_features.statusBarVisible;
			f["toolBarVisible"] = popup_features.toolBarVisible;
			f["scrollbarsVisible"] = popup_features.scrollbarsVisible;
			b["features"] = f;
		}
		set_response(command_fe::popup, b.finalize());
		return true;
	}

	void OnLoadingProgressChange(CefRefPtr<CefBrowser> browser, double progress) override
	{
		mmf->entry->loading_progress = uint16_t(std::max(std::min(progress, 1.), 0.) * UINT16_MAX);
	}

	void update_url(const CefString& url)
	{
		auto str = url.ToString();
		if (str != last_url)
		{
			set_response(command_fe::url, str);
			last_url = std::move(str);
		}
	}

	void OnLoadStart(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, TransitionType transition_type) override
	{
		if (had_error)
		{
			had_error = false;
		}
		if (frame->IsMain())
		{
			update_url(frame->GetURL());

			const auto entry = browser->GetHost()->GetVisibleNavigationEntry();
			lson_builder b;
			if (const auto ssl = entry->GetSSLStatus())
			{
				b.add("secure", ssl->IsSecureConnection() && ssl->GetCertStatus() == 0);
			}
			else
			{
				b.add("secure", false);
			}
			b.add("post", entry->HasPostData());
			b.add("flags", entry->GetTransitionType());
			b.add("status", entry->GetHttpStatusCode());
			set_response(command_fe::load_start, b.finalize());
		}
	}

	void OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, ErrorCode error_code, const CefString& error_text,
		const CefString& failed_url) override
	{
		uint8_t v = 1;
		track_form_data_state.compare_exchange_strong(v, 0);
		if (frame->IsMain() && error_code != ERR_ABORTED)
		{
			set_response(command_fe::load_failed, lson_builder{}
				.add("failedURL", failed_url).add("errorCode", error_code).add("errorText", error_text)
				.finalize());
			update_url(failed_url);
			had_error = true;
		}
	}

	void OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int http_status_code) override
	{
		if (frame->IsMain())
		{
			if (!CefCurrentlyOn(TID_UI))
			{
				assert(0);
				return;
			}
			
			log_message("LOAD END: %d, %d, %d", track_form_data_state.load(), http_status_code, form_data_ready.size());
			if (track_form_data_state != 0)
			{
				if (http_status_code < 400)
				{
					std::unique_lock lock(track_form_mutex);
					if (!form_data_ready.empty())
					{
						set_response(command_fe::form_data, std::move(form_data_ready));
						track_form_data_state = 0;
					}
					else
					{					
						track_form_data_state = 2;
					}
				}
				else
				{
					track_form_data_state = 0;
				}
			}

			const auto entry = browser->GetHost()->GetVisibleNavigationEntry();
			lson_builder b;
			if (const auto ssl = entry->GetSSLStatus())
			{
				b.add("secure", ssl->IsSecureConnection() && ssl->GetCertStatus() == 0);
			}
			else
			{
				b.add("secure", false);
			}
			b.add("post", entry->HasPostData());
			b.add("flags", entry->GetTransitionType());
			b.add("status", entry->GetHttpStatusCode());
			set_response(command_fe::load_end, b.finalize());
		}
	}

	d3d11::Texture2D* texture(const d3d11::Context& ctx)
	{
		assert(view_buffer_);
		return view_buffer_->swap(ctx);
	}

	bool loaded_resources_monitor{};
	bool loaded_resources_filter{};
	bool use_custom_headers{};
	bool redirect_navigation{};
	bool keep_suspended_texture{};
	bool had_error{};
	std::mutex resources_filter_mutex;
	std::mutex custom_headers_mutex;
	std::regex resources_filter;
	std::vector<std::pair<std::regex, std::vector<std::pair<CefString, CefString>>>> custom_headers;

	void OnScrollOffsetChanged(CefRefPtr<CefBrowser> browser, double x, double y) override
	{
		mmf->entry->scroll_x = float(x);
		mmf->entry->scroll_y = float(y);
	}

	static bool test_regex(const std::string& s, const std::regex& r)
	{
		return r._Empty() || std::regex_search(s, r);
	}

	ReturnValue OnBeforeResourceLoad(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> request, CefRefPtr<CefCallback> callback) override
	{
		if (loaded_resources_filter)
		{
			std::unique_lock lock(resources_filter_mutex);
			if (test_regex(request->GetURL().ToString(), resources_filter))
			{
				if (loaded_resources_monitor)
				{
					set_response(command_fe::url_monitor, request->GetURL().ToString() + '\1' + '1');
				}
				return RV_CANCEL;
			}
			if (loaded_resources_monitor)
			{
				set_response(command_fe::url_monitor, request->GetURL());
			}
		}
		else if (loaded_resources_monitor)
		{
			set_response(command_fe::url_monitor, request->GetURL());
		}
		if (use_custom_headers)
		{
			std::unique_lock lock(custom_headers_mutex);
			const auto url = request->GetURL().ToString();
			for (auto& i : custom_headers)
			{
				if (test_regex(url, i.first))
				{
					for (auto& p : i.second)
					{
						request->SetHeaderByName(p.first, p.second, true);
					}
				}
			}
		}
		return RV_CONTINUE;
	}

	CefRefPtr<CefResourceRequestHandler> GetResourceRequestHandler(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
		CefRefPtr<CefRequest> request, bool is_navigation, bool is_download, const CefString& request_initiator, bool& disable_default_handling) override
	{
		return this;
	}

	bool use_injection{};
	std::mutex injection_mutex;
	std::vector<std::pair<std::regex, std::string>> injection_entries_css;
	std::vector<std::pair<std::regex, std::string>> injection_entries_js;

	struct TargettedResourceFilter : CefResponseFilter
	{
		std::string injection_data;
		std::string injection_combined;
		uint64_t injection_combined_pos{};

		TargettedResourceFilter(std::string data) : injection_data(std::move(data)) {}
		bool InitFilter() override { return true; }

		FilterStatus Filter(void* data_in, size_t data_in_size, size_t& data_in_read,
			void* data_out, size_t data_out_size, size_t& data_out_written) override
		{
			if (data_in_size == 0)
			{
				if (injection_combined_pos > 0)
				{
					data_in_read = 0;
					const auto left = injection_combined.size() - injection_combined_pos;
					if (left <= data_out_size)
					{
						memcpy(data_out, injection_combined.data() + injection_combined_pos, left);
						data_out_written = left;
						injection_combined_pos = 0;
						return RESPONSE_FILTER_DONE;
					}

					memcpy(data_out, injection_combined.data() + injection_combined_pos, data_out_size);
					injection_combined_pos += data_out_size;
					data_out_written = data_out_size;
					return RESPONSE_FILTER_NEED_MORE_DATA;
				}
				data_in_read = 0;
				data_out_written = 0;
				return RESPONSE_FILTER_DONE;
			}

			const auto s = utils::str_view((const char*)data_in, 0, data_in_size);
			data_in_read = data_in_size;

			const auto f = s.find("</head>");
			if (f == std::string::npos)
			{
				const auto size = std::min(data_in_size, data_out_size);
				memcpy(data_out, data_in, size);
				data_in_read = size;
				data_out_written = size;
			}
			else
			{
				auto new_size = data_in_size + injection_data.size();
				if (new_size <= data_out_size)
				{
					memcpy(data_out, data_in, f);
					memcpy((void*)((const char*)data_out + f), injection_data.data(), injection_data.size());
					memcpy((void*)((const char*)data_out + f + injection_data.size()), s.data() + f, data_in_size - f);
					data_out_written = new_size;
				}
				else
				{
					if (f <= data_out_size)
					{
						memcpy(data_out, data_in, f);
						injection_combined.resize(new_size - f);
						memcpy(injection_combined.data(), injection_data.data(), injection_data.size());
						memcpy(injection_combined.data() + injection_data.size(), s.data() + f, data_in_size - f);
						memcpy((void*)((const char*)data_out + f), injection_combined.data(), data_out_size - f);
						injection_combined_pos = data_out_size - f;
					}
					else
					{
						injection_combined.resize(new_size);
						memcpy(injection_combined.data(), data_in, f);
						memcpy(injection_combined.data() + f, injection_data.data(), injection_data.size());
						memcpy(injection_combined.data() + f + injection_data.size(), s.data() + f, data_in_size - f);
						memcpy(data_out, injection_combined.data(), data_out_size);
						injection_combined_pos = data_out_size;
					}
					data_out_written = data_out_size;
					return RESPONSE_FILTER_NEED_MORE_DATA;
				}
			}

			return RESPONSE_FILTER_DONE;
		}

	private:
		IMPLEMENT_REFCOUNTING(TargettedResourceFilter);
	};

	constexpr static size_t injection_css_prefix = std::char_traits<char>::length("<style>");
	constexpr static size_t injection_js_prefix = std::char_traits<char>::length("<script>");
	std::string injection_collected_css{"<style>"};
	std::string injection_collected_js{"<script>"};

	CefRefPtr<CefResponseFilter> GetResourceResponseFilter(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> request,
		CefRefPtr<CefResponse> response) override
	{
		if (use_injection && response->GetMimeType() == "text/html")
		{
			const auto url = request->GetURL().ToString();
			if (!utils::str_view::from_str(url).ends_with_ci(".js"))
			{
				std::unique_lock lock(injection_mutex);
				injection_collected_css.resize(injection_css_prefix);
				injection_collected_js.resize(injection_js_prefix);
				for (auto& i : injection_entries_css)
				{
					if (test_regex(url, i.first))
					{
						injection_collected_css += i.second;
					}
				}
				for (auto& i : injection_entries_js)
				{
					if (test_regex(url, i.first))
					{
						injection_collected_js += i.second;
						injection_collected_js.push_back(';');
					}
				}
				if (!color_scheme_active.empty() || injection_collected_css.size() > injection_css_prefix || injection_collected_js.size() > injection_js_prefix)
				{
					std::string result;
					result.reserve(color_scheme_active.size() + injection_collected_css.size() + injection_collected_js.size() 
						+ injection_css_prefix + injection_js_prefix + 2);
					result = color_scheme_active;
					result += injection_collected_css;
					result += "</style>";
					result += injection_collected_js;
					result += "</script>";
					return new TargettedResourceFilter(std::move(result));
				}
			}
		}
		return nullptr;
	}
	
	std::mutex redirect_nonstandard_schemes_mutex;
	std::regex redirect_nonstandard_schemes_filter;

	bool button_ctrl{};
	bool button_shift{};
	bool button_alt{};

	std::string initial_url = "about:blank";
	
	std::mutex ignore_certificate_errors_mutex;
	std::regex ignore_certificate_errors_filter;

	bool verify_full_access(const char* reason)
	{
		return has_full_access_;
	}

	static std::regex create_regex(const utils::str_view& value)
	{
		try
		{
			return value.empty() ? std::regex{} : std::regex{value.str(), std::regex_constants::icase | std::regex_constants::optimize | std::regex_constants::nosubs};
		}
		catch (std::exception& e)
		{
			std::cout << "Incorrect regex: " << value.str() << ", " << e.what() << std::endl;
			return std::regex{"^___never_{1024}shouldneverhappen__$"};
		}
	}

	bool configure_control(command_be key, const utils::str_view& value)
	{
		if (key == command_be::navigate)
		{
			initial_url = value.str();
		}
		else if (key == command_be::set_option)
		{
			const auto kv = value.pair('\1');
			if (kv.first == "ignoreCertificateErrors")
			{
				std::unique_lock lock(ignore_certificate_errors_mutex);
				ignore_certificate_errors_filter = create_regex(kv.second);
			}
			else if (kv.first == "trackFormData")
			{
				if (verify_full_access("Track form data"))
				{
					track_form_data = kv.second == "1";
				}
			}
			else if (kv.first == "redirectNavigation")
			{
				redirect_navigation = kv.second == "1";
			}
			else if (kv.first == "redirectNonStandardSchemes")
			{
				if (verify_full_access("Redirect non-standard schemes"))
				{
					std::unique_lock lock(redirect_nonstandard_schemes_mutex);
					redirect_nonstandard_schemes_filter = create_regex(kv.second);
				}
			}
			else if (kv.first == "collectResourceURLs")
			{
				if (verify_full_access("Collect URLs"))
				{
					loaded_resources_monitor = kv.second == "1";
				}
			}
			else if (kv.first == "keepSuspendedTexture")
			{
				keep_suspended_texture = kv.second == "1";
			}
			else if (kv.first == "scaleFactor")
			{
				scale_factor = kv.second.as(1.f);
				if (const auto browser = safe_browser())
				{
					browser->GetHost()->NotifyScreenInfoChanged();
					browser->GetHost()->WasResized();
				}
			}
			else if (kv.first == "invalidateView")
			{
				const auto x = width_, y = height_;
				resize(x + 1, y);
				CefPostDelayedTask(TID_UI, new BasicTask([=]
				{
					resize(x, y);
				}), 20);
			}
			else
			{
				std::cout << "Unknown option: " << kv.first.str();
			}
		}
		else if (key == command_be::filter_resource_urls)
		{
			loaded_resources_filter = false;
			if (!value.empty())
			{
				std::unique_lock lock(resources_filter_mutex);
				resources_filter = create_regex(value);
				loaded_resources_filter = true;
			}
		}
		else if (key == command_be::set_headers)
		{
			if (verify_full_access("Set headers"))
			{
				const auto table = value.pairs('\1');
				std::unique_lock lock(custom_headers_mutex);
				custom_headers.clear();
				for (const auto& p : table)
				{
					std::vector<std::pair<CefString, CefString>> headers;
					for (const auto& i : p.second.pairs('\2'))
					{
						headers.emplace_back(i.first, i.second);
					}
					custom_headers.emplace_back(create_regex(p.first), std::move(headers));
				}
				use_custom_headers = !custom_headers.empty();
			}
		}
		else if (key == command_be::inject_css)
		{
			const auto table = value.pairs('\1');
			std::unique_lock lock(injection_mutex);
			injection_entries_css.clear();
			for (const auto& i : table)
			{
				injection_entries_css.emplace_back(create_regex(i.first), i.second.str());
				for (auto& c : injection_entries_css.back().second)
				{
					if (c == '<' && _strnicmp(&c, "</style", 7) == 0) c = '?';
				}
			}
			use_injection = !injection_entries_css.empty() || !injection_entries_js.empty() || !color_scheme_active.empty();
		}
		else if (key == command_be::inject_js)
		{
			if (verify_full_access("Inject JS"))
			{
				const auto table = value.pairs('\1');
				std::unique_lock lock(injection_mutex);
				injection_entries_js.clear();
				for (const auto& i : table)
				{
					if (i.second.find("</script>") != std::string::npos) continue;
					injection_entries_js.emplace_back(create_regex(i.first), i.second.str());
				}
				use_injection = !injection_entries_css.empty() || !injection_entries_js.empty() || !color_scheme_active.empty();
			}
		}
		else
		{
			return false;
		}
		return true;
	}

	struct string_visitor_with_callback : CefStringVisitor
	{
		std::function<void(std::string&)> callback;
		std::string ret;

		string_visitor_with_callback(std::function<void(std::string&)> fn) : callback(std::move(fn)) { }
		~string_visitor_with_callback() override { callback(ret); }
		void Visit(const CefString& string) override { ret += string.ToString(); }

	private:
		IMPLEMENT_REFCOUNTING(string_visitor_with_callback);
	};

	CefRefPtr<CefFindHandler> GetFindHandler() override
	{
		return this;
	}

	// bool OnQuotaRequest(CefRefPtr<CefBrowser> browser, const CefString& origin_url, int64 new_size, CefRefPtr<CefCallback> callback) override;

	void OnFindResult(CefRefPtr<CefBrowser> browser, int identifier, int count, const CefRect& selection_rect, int active_match_ordinal, bool final_update) override
	{
		set_response(command_fe::found_result, lson_builder{}
			.add("identifier", identifier)
			.add("index", active_match_ordinal)
			.add("count", count)
			.add("rect", lson_builder{}
				.add("x", selection_rect.x)
				.add("y", selection_rect.y)
				.add("width", selection_rect.width)
				.add("height", selection_rect.height))
			.add("final", final_update)
			.finalize());
	}

	static lson_builder issuer_data(const CefRefPtr<CefX509CertPrincipal>& i)
	{
		lson_builder c;
		c.add("commonName", i->GetCommonName());
		{
			lson_builder n;
			std::vector<CefString> organizations;
			i->GetOrganizationNames(organizations);
			for (const auto& o : organizations) n.add(nullptr, o);
			c.add("organizationNames", n);
		}
		{
			lson_builder n;
			std::vector<CefString> organizations;
			i->GetOrganizationUnitNames(organizations);
			for (const auto& o : organizations) n.add(nullptr, o);
			c.add("organizationUnitNames", n);
		}
		return c;
	}

	void apply_scroll(bool absolute, int32_t x, int32_t y)
	{
		const auto browser = safe_browser();
		if (browser)
		{
			auto command = strformat("scroll%s(%d, %d)", absolute ? "To" : "By", x, y);
			browser->GetMainFrame()->ExecuteJavaScript(command, "", 0);
		}
	}

	struct
	{
		bool absolute = true;
		int x = INT32_MAX;
		int y = 0;
	} postponed_scroll;
	uint8_t notify_resized = 4;
	bool close_command_sent_{};
	bool stay_suspended_{};
	bool graduate_close_{};

	bool DoClose(CefRefPtr<CefBrowser> browser) override
	{
		if (browser_ptr_.load() && !close_command_sent_)
		{
			set_response(command_fe::close, std::string());
			close_command_sent_ = true;
		}
		if (graduate_close_)
		{
			suspended = true;
			browser->GetMainFrame()->SendProcessMessage(PID_RENDERER, CefProcessMessage::Create(PMSG_KILL));
		}
		return graduate_close_;
	}

	void OnBeforeClose(CefRefPtr<CefBrowser>) override
	{
		log_message("WebView::OnBeforeClose(%p): ref_count=%d, browser=%p", this, (*(base::AtomicRefCount*)&ref_count_).SubtleRefCountForDebug(), browser_ptr_.load());
		if (const auto browser = browser_ptr_.exchange(nullptr))
		{
			auto release = browser->Release();
			log_message("browser->Release(): %d", release); 
		}
	}

	time_t last_crash_time{};
	uint32_t crash_counter{};

	void OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser, TerminationStatus status) override
	{
		if (suspended) 
		{
			if (passthrough_mode_)
			{
				if (keep_suspended_texture)
				{
					pd_main.clean();
				}
				else
				{	
					mmf->entry->handle = 0ULL;
					pd_main.reset();			
				}
				popup_active = false;
				mmf->entry->popup_handle = 0ULL;
				pd_popup.reset();
			}
			return;
		}

		const auto now = time(nullptr);
		std::cout << "Render process crashed: " << int(status) << " (counter: " << crash_counter << ", time: " << now - last_crash_time << ")" << std::endl;
		if (now - last_crash_time > 30)
		{
			last_crash_time = now;
			crash_counter = 0;
		}
		else if (++crash_counter > 7) // 8 crashes in a half a minute
		{
			std::quick_exit(29);
		}

		browser->Reload();
	}

	std::string color_scheme_active;
	uint32_t own_zoom_phase{};
	bool dark_auto_active{};
	bool dark_forced_active{};

	template<typename T>
	static void do_on_ui(T&& fn, bool allow_immediate)
	{
		if (_cef_thread || !allow_immediate)
		{
			CefPostTask(TID_UI, new BasicTask(fn));
		}
		else
		{
			fn();
		}
	}

	void control(command_be key, const utils::str_view& value)
	{
		if (key == command_be::large_command)
		{
			const auto file_key = *(uint32_t*)&value[0];
			const auto data_size = *(uint32_t*)&value[4];
			if (data_size > 0)
			{
				try
				{
					const auto file_filename = named_prefix + L'_' + std::to_wstring(file_key);
					const auto data = accsp_mapped(file_filename, data_size);
					control(*(command_be*)data.entry, data.view().substr(1, data_size - 1));
				}
				catch (std::exception& e)
				{
					std::cout << "Failed to read large command: " << e.what() << std::endl;
				}
			}
			return;
		}

		const auto browser = safe_browser();
		if (!browser)
		{
			return;
		}

		if (key == command_be::navigate)
		{
			if (suspended) return;
			if (value == "back")
			{
				browser->GoBack();
			}
			else if (value == "forward")
			{
				browser->GoForward();
			}
			else if (value.starts_with("back:") || value.starts_with("forward:"))
			{
				for (auto i = std::stoull(value.pair(':').second.str()); i > 0; --i)
				{
					value[0] == 'b' ? browser->GoBack() : browser->GoForward();
				}
			}
			else if (!value.starts_with_ci("javascript:") || verify_full_access("Navigate to JavaScript URLs"))
			{
				browser->GetMainFrame()->LoadURL(value);
			}
		}
		else if (key == command_be::zoom)
		{
			if (!browser->HasDocument())
			{
				postponed_zoom = value.as(0.f);
				mmf->entry->zoom_level = postponed_zoom;
			}
			else
			{
				do_on_ui([browser = CefRefPtr(browser), that = CefRefPtr(this), zoom_value = value.as(0.f)]
				{
					browser->GetHost()->SetZoomLevel(zoom_value);
					that->own_zoom_phase = ++_zoom_phase;
					that->mmf->entry->zoom_level = zoom_value;
				}, true);
			}
		}
		else if (key == command_be::reload)
		{
			if (suspended) return;
			if (last_browse_nonget) browser->GetMainFrame()->ExecuteJavaScript("location.reload()", "", 0);
			else if (value == "nocache") browser->ReloadIgnoreCache();
			else browser->Reload();
		}
		else if (key == command_be::stop)
		{
			if (suspended) return;
			browser->StopLoad();
		}
		else if (key == command_be::download)
		{
			browser->GetHost()->StartDownload(value);
		}
		else if (key == command_be::lifespan)
		{
			if (value == "close")
			{
				graduate_close_ = true;
				browser->GetHost()->GetRequestContext()->GetCookieManager(nullptr)->FlushStore(nullptr);
				browser->GetHost()->CloseBrowser(false); 
			}
			else if (value == "suspend" || value == "resume")
			{
				suspended = value == "suspend";
				if (suspended)
				{
					browser->GetMainFrame()->SendProcessMessage(PID_RENDERER, CefProcessMessage::Create(PMSG_KILL));
				}
				else
				{
					close_command_sent_ = false;
					browser->Reload();
				}
			}
		}
		else if (key == command_be::command)
		{
			if (value == "undo") browser->GetFocusedFrame()->Undo();
			else if (value == "redo") browser->GetFocusedFrame()->Redo();
			else if (value == "copy") browser->GetFocusedFrame()->Copy();
			else if (value == "cut") browser->GetFocusedFrame()->Cut();
			else if (value == "paste") browser->GetFocusedFrame()->Paste();
			else if (value == "delete") browser->GetFocusedFrame()->Delete();
			else if (value == "selectAll") browser->GetFocusedFrame()->SelectAll();
			else if (value == "print" && verify_full_access("Print")) browser->GetHost()->Print();
			else if (value == "exitFullscreen") browser->GetMainFrame()->ExecuteJavaScript("document.webkitExitFullscreen()", "", 0);
		}
		else if (key == command_be::input)
		{
			cef_string_wide_t wide{};
			cef_string_utf8_to_wide(value.data(), value.size(), &wide);

			for (auto i = 0U; i < wide.length; ++i)
			{
				CefKeyEvent e;
				e.type = KEYEVENT_CHAR;
				e.native_key_code = 0;
				e.windows_key_code = wide.str[i];
				e.modifiers = 0;
				e.character = 0;
				e.unmodified_character = 0;
				e.focus_on_editable_field = true;
				browser->GetHost()->SendKeyEvent(e);
			}
			if (wide.dtor)
			{
				wide.dtor(wide.str);
			}
		}
		else if (key == command_be::key_down || key == command_be::key_up)
		{
			const auto repeated = value[0] == '\1';
			CefKeyEvent e;
			e.type = key == command_be::key_down ? KEYEVENT_KEYDOWN : KEYEVENT_KEYUP;
			e.windows_key_code = repeated ? value.substr(1).as(0) : value.as(0);
			e.native_key_code = MapVirtualKeyA(e.windows_key_code, MAPVK_VK_TO_VSC);
			e.character = 0;
			e.unmodified_character = 0;
			e.is_system_key = false;
			e.focus_on_editable_field = false;
			e.modifiers = get_event_flags() | (repeated ? EVENTFLAG_IS_REPEAT : 0);
			if (e.windows_key_code == VK_CONTROL) button_ctrl = e.type == KEYEVENT_KEYDOWN;
			if (e.windows_key_code == VK_SHIFT) button_shift = e.type == KEYEVENT_KEYDOWN;
			if (e.windows_key_code == VK_MENU) button_alt = e.type == KEYEVENT_KEYDOWN;
			browser->GetHost()->SendKeyEvent(e);
		}
		else if (key == command_be::find)
		{
			if (value.size() > 3)
			{
				browser->GetHost()->Find(value.substr(3), value[0] == '1', value[1] == '1', value[2] == '1');
			}
			else
			{
				browser->GetHost()->Find(CefString("\0", 1), true, false, false);
			}
		}
		else if (key == command_be::download_image)
		{
			struct favicon_callback : CefDownloadImageCallback
			{
				WebView* parent;
				std::string key;

				favicon_callback(WebView* parent, const utils::str_view& key) : parent(parent), key(key.str()) {}

				void OnDownloadImageFinished(const CefString& image_url, int http_status_code, CefRefPtr<CefImage> image) override
				{
					if (!image || image->IsEmpty())
					{
						parent->set_reply(std::move(key), std::string());
						return;
					}

					int w, h;
					auto b = image->GetAsPNG(1.f, true, w, h);
					std::string ret;
					ret.resize(b->GetSize());
					b->GetData(ret.data(), ret.size(), 0);
					parent->set_reply(std::move(key), std::move(ret));
				}

			private:
				IMPLEMENT_REFCOUNTING(favicon_callback);
			};
			auto p = value.split('\1', false, false); // reply, URL, favicon, max_size
			browser->GetHost()->DownloadImage(p[1], p[2] == "1", p[3].as(0), false, new favicon_callback(this, p[0]));
		}
		else if (key == command_be::mute)
		{
			if (!redirect_audio_) browser->GetHost()->SetAudioMuted(value == "1");
			if (value == "1") base_flags |= 16ULL;
			else base_flags &= ~16ULL;
		}
		else if (key == command_be::scroll)
		{
			auto p = value.split('\1', false, false);
			assert(p.size() == 3);
			if (p.size() == 3)
			{
				auto absolute = p[0] == "1";
				auto x = p[1].as(0);
				auto y = p[2].as(0);
				if (!last_title.empty() && mmf->entry->loading_progress == 65535)
				{
					apply_scroll(absolute, x, y);
				}
				else
				{
					postponed_scroll.absolute = absolute;
					postponed_scroll.x = x;
					postponed_scroll.y = y;
				}
			}
		}
		else if (key == command_be::capture_lost)
		{
			browser->GetHost()->SendCaptureLostEvent();
		}
		else if (key == command_be::execute)
		{
			if (verify_full_access("Execute JavaScript"))
			{
				browser->GetMainFrame()->ExecuteJavaScript(value, "", 0);
			}
		}
		else if (key == command_be::dev_tools_message)
		{
			const auto kv = value.pair('\1');
			for (auto c : kv.first)
			{
				if (c == '"' || c == '\\' || c == '\n' || c == '\r')
				{
					std::cout << "Damaged command: " << kv.first.str() << std::endl;
					return;
				}
			}
			if (kv.first.starts_with("Emulation.")
				|| kv.first.starts_with("Overlay.")
				|| kv.first == "Network.emulateNetworkConditions"
				|| verify_full_access("Advanced DevTools message"))
			{
				std::string packet("{\"id\":0,\"method\":\"");
				packet += kv.first.str();
				packet += "\",\"params\":";
				packet += kv.second.str();
				packet.push_back('}');
				do_on_ui([browser = CefRefPtr(browser), str = std::move(packet)]
				{
					browser->GetHost()->SendDevToolsMessage(str.data(), str.size());
				}, false);
			}
		}
		else if (key == command_be::color_scheme)
		{
			if (value == "dark-auto")
			{
				if (!dark_auto_active)
				{
					do_on_ui([browser = CefRefPtr(browser)]
					{
						const auto c1 = "{\"id\":0,\"method\":\"Emulation.setEmulatedMedia\",\"params\":{\"features\":[{\"name\":\"prefers-color-scheme\",\"value\":\"dark\"}]}}";
						const auto c2 = "{\"id\":0,\"method\":\"Emulation.setAutoDarkModeOverride\",\"params\":{\"enabled\":true}}";
						browser->GetHost()->SendDevToolsMessage(c1, strlen(c1));
						browser->GetHost()->SendDevToolsMessage(c2, strlen(c2));
					}, false);
				}
				dark_auto_active = true;
			}
			else
			{
				std::string packet("{\"id\":0,\"method\":\"Emulation.setEmulatedMedia\",\"params\":{\"features\":[{\"name\":\"prefers-color-scheme\",\"value\":\"");
				packet += value == "dark-forced" ? utils::str_view::from_cstr("dark") : value;
				packet += "\"}]}}";
				do_on_ui([browser = CefRefPtr(browser), str = std::move(packet), dark_auto_active = dark_auto_active]
				{
					browser->GetHost()->SendDevToolsMessage(str.data(), str.size());
					if (dark_auto_active)
					{
						const auto c2 = "{\"id\":0,\"method\":\"Emulation.setAutoDarkModeOverride\",\"params\":{\"enabled\":false}}";
						browser->GetHost()->SendDevToolsMessage(c2, strlen(c2));
					}
				}, false);
				dark_auto_active = false;
			}

			std::string injection;
			if (value == "dark-forced" || dark_forced_active)
			{
				injection = value != "dark-forced" ? "" : "<style __data_csp_color_scheme=1>input,label,select,textarea,button,fieldset,legend,datalist,output,option,optgroup{"
					"color-scheme:dark;}</style><meta __data_csp_color_scheme=1 name=\"color-scheme\" content=\"dark\">";
				auto injection_js = value != "dark-forced"
					? "[].forEach.call(document.querySelectorAll('[__data_csp_color_scheme]'), x => x.parentNode.removeChild(x))"
					: "[].forEach.call(document.querySelectorAll('[__data_csp_color_scheme]'), x => x.parentNode.removeChild(x));"
						"document.head.insertAdjacentHTML('beforeend','" + injection + "')";
				std::vector<int64_t> frames;
				browser->GetFrameIdentifiers(frames);
				for (auto f : frames)
				{
					if (auto frame = browser->GetFrame(f))
					{
						frame->ExecuteJavaScript(injection_js, "", 0);
					}
				}
				dark_forced_active = value == "dark-forced";
			}

			{
				std::unique_lock lock(injection_mutex);
				color_scheme_active = std::move(injection);
				use_injection = !injection_entries_css.empty() || !injection_entries_js.empty() || !color_scheme_active.empty();
			}
		}
		else if (key == command_be::control_download)
		{
			cancel_download(value.pair('\1'));
		}
		else if (key == command_be::awake)
		{
			visible_counter = 250;
			update_visible_state();
		}
		else if (key == command_be::fill_form)
		{			
			if (verify_full_access("Fill form"))
			{
				auto message = CefProcessMessage::Create(PMSG_FILL_FORM);
				const auto args = message->GetArgumentList();
				auto i = 0;
				for (auto p : value.split('\1', false, false))
				{
					args->SetString(i++, p.str());
				}
				browser->GetMainFrame()->SendProcessMessage(PID_RENDERER, std::move(message));
			}
		}
		else if (key == command_be::html)
		{
			if (verify_full_access("Get HTML"))
			{
				browser->GetMainFrame()->GetSource(new string_visitor_with_callback([this, key = value.str()](std::string& data)
				{
					set_reply(key, data);
				}));
			}
			else
			{
				set_reply(value.str(), "");
			}
		}
		else if (key == command_be::text)
		{
			if (verify_full_access("Get text"))
			{
				browser->GetMainFrame()->GetText(new string_visitor_with_callback([this, key = value.str()](std::string& data)
				{
					set_reply(key, data);
				}));
			}
			else
			{
				set_reply(value.str(), "");
			}
		}
		else if (key == command_be::write_cookies)
		{
			if (verify_full_access("Edit cookies"))
			{
				const auto args = value.split('\1', false, false);
				const auto cookies = browser->GetHost()->GetRequestContext()->GetCookieManager(nullptr);
				if (args.size() <= 2)
				{
					if (args[0] == "@recent")
					{
						if (args.size() != 2) return;
						struct recent_cookie_visitor : CefCookieVisitor
						{
							time_t time_threshold;

							recent_cookie_visitor(WebView* parent, int max_age)
							{
								time_threshold = time(nullptr) - max_age;
							}

							bool Visit(const CefCookie& cookie, int count, int total, bool& delete_cookie) override
							{
								time_t time_la, time_c;
								cef_time_to_timet(&cookie.last_access, &time_la);
								cef_time_to_timet(&cookie.creation, &time_c);
								delete_cookie = std::max(time_la, time_c) > time_threshold;
								return true;
							}

						private:
							IMPLEMENT_REFCOUNTING(recent_cookie_visitor);
						};
						cookies->VisitAllCookies(new recent_cookie_visitor(this, args[1].as(0)));
					}
					else
					{
						cookies->DeleteCookies(args[0], args.size() == 2 ? args[1] : utils::str_view{}, nullptr);
					}
				}
				else
				{
					auto get_cef_time = [](const utils::str_view& v)
					{
						cef_time_t ret{};
						cef_time_from_timet(v.as(0ULL), &ret);
						return ret;
					};
					CefCookie cookie{};
					cookie.name = args[1];
					for (auto& p : args[2].pairs('\2'))
					{
						if (p.first == "value") cookie.value = p.second;
						else if (p.first == "domain") cookie.domain = p.second;
						else if (p.first == "path") cookie.path = p.second;
						else if (p.first == "secure") cookie.secure = p.second == "1" ? 1 : 0;
						else if (p.first == "HTTPOnly") cookie.httponly = p.second == "1" ? 1 : 0;
						else if (p.first == "creationTime") cookie.creation = get_cef_time(p.second);
						else if (p.first == "lastAccessTime") cookie.last_access = get_cef_time(p.second);
						else if (p.first == "expirationTime")
						{
							cookie.expires = get_cef_time(p.second);
							cookie.has_expires = 1;
						}
					}
					cookies->SetCookie(args[0], cookie, nullptr);
				}
			}
		}
		else if (key == command_be::read_cookies)
		{
			struct cookie_visitor : CefCookieVisitor
			{
				WebView* parent;
				std::string key;
				lson_builder b;
				uint8_t mode;

				cookie_visitor(WebView* parent, const utils::str_view& key, const utils::str_view& mode_key) : parent(parent), key(key.str())
				{
					if (mode_key == "basic") mode = 1;
					else if (mode_key == "count") mode = 2;
					else mode = 0;
				}

				~cookie_visitor() override
				{
					if (key.empty()) return;
					if (mode == 2)
					{
						parent->set_reply(std::move(key), "0");
					}
					else
					{
						parent->set_reply(std::move(key), b.finalize());
					}
				}

				bool Visit(const CefCookie& cookie, int count, int total, bool& delete_cookie) override
				{
					if (mode == 2)
					{
						parent->set_reply(std::move(key), std::to_string(total));
						key.clear();
						return false;
					}
					auto c = lson_builder{}
						.add("name", cookie.name)
						.add("value", cookie.value);
					if (mode == 0)
					{
						c.add_opt("domain", cookie.domain)
							.add_opt("path", cookie.path)
							.add("secure", cookie.secure != 0)
							.add("HTTPOnly", cookie.httponly != 0)
							.add("creationTime", cookie.creation)
							.add("lastAccessTime", cookie.last_access);
						if (cookie.has_expires != 0)
						{
							c.add("expirationTime", cookie.expires);
						}
					}
					b.add(nullptr, c);
					return true;
				}

			private:
				IMPLEMENT_REFCOUNTING(cookie_visitor);
			};
			const auto args = value.split('\1', false, false);
			const auto cookies = browser->GetHost()->GetRequestContext()->GetCookieManager(nullptr);
			if (args[1] != "count" && !verify_full_access("Read cookies"))
			{
				set_reply(args[0].str(), "{}");
				return;
			}
			if (args[2].empty())
			{
				cookies->VisitAllCookies(new cookie_visitor(this, args[0], args[1]));
			}
			else
			{
				cookies->VisitUrlCookies(args[2], false, new cookie_visitor(this, args[0], args[1]));
			}
		}
		else if (key == command_be::history)
		{
			const auto args = value.pair('\1');
			if (auto f = args.second == "forward"; f || args.second == "back")
			{
				struct history_visitor_d : CefNavigationEntryVisitor
				{
					struct entry_holder
					{
						std::string display_url;
						std::string title;
						int http_status_code;
						cef_transition_type_t transition_type;
						bool has_post_data;
						bool current;

						entry_holder(const CefRefPtr<CefNavigationEntry>& e, bool current) : current(current)
						{
							display_url = e->GetDisplayURL();
							title = e->GetTitle();
							http_status_code = e->GetHttpStatusCode();
							transition_type = e->GetTransitionType();
							has_post_data = e->HasPostData();
						}

						explicit operator lson_builder() const
						{
							lson_builder b;
							b.add("current", current);
							b.add("displayURL", display_url);
							b.add("title", title);
							b.add("hasPostData", has_post_data);
							b.add("HTTPCode", http_status_code);
							b.add("transitionType", transition_type);
							return b;
						}
					};

					WebView* parent;
					std::string key;
					std::vector<std::unique_ptr<entry_holder>> entries;
					bool forward;
					bool found_current{};

					history_visitor_d(WebView* parent, std::string key, bool forward)
						: parent(parent), key(std::move(key)), forward(forward) {}

					~history_visitor_d() override
					{
						lson_builder ret;
						if (forward)
						{
							for (const auto& e : entries)
							{
								ret.add(nullptr, lson_builder(*e));
							}
						}
						else
						{
							for (auto i = entries.size(), j = 0ULL; i > 0 && j < 10; --i, ++j)
							{
								ret.add(nullptr, lson_builder(*entries[i - 1]));
							}
						}
						parent->set_reply(std::move(key), ret.finalize());
					}

					bool Visit(CefRefPtr<CefNavigationEntry> entry, bool current, int index, int total) override
					{
						if (forward)
						{
							if (found_current)
							{
								entries.push_back(std::make_unique<entry_holder>(entry, current));
								return entries.size() < 10 && index + 1 < total;
							}
							if (current)
							{
								found_current = true;
							}
						}
						else
						{
							if (current)
							{
								return false;
							}
							entries.push_back(std::make_unique<entry_holder>(entry, current));							
						}
						return index + 1 < total;
					}

				private:
					IMPLEMENT_REFCOUNTING(history_visitor_d);
				};
				browser->GetHost()->GetNavigationEntries(new history_visitor_d(this, args.first.str(), f), false);
			}
			else
			{
				struct history_visitor_b : CefNavigationEntryVisitor
				{
					WebView* parent;
					std::string key;
					lson_builder ret;

					history_visitor_b(WebView* parent, std::string key) : parent(parent), key(std::move(key)) {}
					~history_visitor_b() override { parent->set_reply(std::move(key), ret.finalize()); }

					bool Visit(CefRefPtr<CefNavigationEntry> entry, bool current, int index, int total) override
					{
						lson_builder b;
						b.add("current", current);
						b.add("displayURL", entry->GetDisplayURL());
						b.add("title", entry->GetTitle());
						b.add("hasPostData", entry->HasPostData());
						b.add("HTTPCode", entry->GetHttpStatusCode());
						b.add("transitionType", entry->GetTransitionType());
						ret.add(nullptr, b);
						return index + 1 < total;
					}

				private:
					IMPLEMENT_REFCOUNTING(history_visitor_b);
				};
				browser->GetHost()->GetNavigationEntries(new history_visitor_b(this, args.first.str()), false);
			}
		}
		else if (key == command_be::ssl)
		{
			do_on_ui([that = CefRefPtr(this), browser = CefRefPtr(browser), key = value.str()]() mutable
			{
				const auto entry = browser->GetHost()->GetVisibleNavigationEntry();
				lson_builder b;
				if (auto ssl = entry->GetSSLStatus())
				{
					b.add("secure", ssl->IsSecureConnection());
					b.add("faultsMask", (ssl->GetCertStatus()));
					b.add("SSLVersion", ssl->GetSSLVersion());
					if (auto cert = ssl->GetX509Certificate())
					{
						b.add("certificate", lson_builder{}
							.add("validPeriod", lson_builder{}
								.add("creation", cert->GetValidStart().GetTimeT())
								.add("expiration", cert->GetValidExpiry().GetTimeT()))
							.add("issuer", issuer_data(cert->GetIssuer()))
							.add("subject", issuer_data(cert->GetSubject()))
							.add("chainSize", cert->GetIssuerChainSize()));
					}
				}
				that->set_reply(std::move(key), b.finalize());
			}, true);
		}
		else if (key == command_be::send)
		{
			auto message = CefProcessMessage::Create(PMSG_RECEIVE_IN);
			const auto kv = value.split('\1', false, false, 3);
			const auto args = message->GetArgumentList();
			args->SetString(0, kv[0]);
			args->SetString(1, kv[1]);
			args->SetString(2, kv[2]);
			browser->GetMainFrame()->SendProcessMessage(PID_RENDERER, std::move(message));
		}
		else if (key == command_be::reply)
		{
			const auto kv = value.pair('\1');
			const auto i = std::stoull(kv.first.str());
			const auto f = awaiting_reply.find(i);
			if (f != awaiting_reply.end())
			{
				auto fn = std::move(f->second);
				awaiting_reply.erase(f);
				fn(kv.second);
			}
		}
		else
		{
			configure_control(key, value);
		}
	}

	void OnFullscreenModeChange(CefRefPtr<CefBrowser> browser, bool fullscreen) override
	{
		fullscreen_active = fullscreen;
	}

	bool fullscreen_active{};
	uint16_t last_mouse_x{};
	uint16_t last_mouse_y{};
	uint16_t last_mouse_flags{};
	std::string last_url;

	std::mutex response_mutex;
	std::vector<std::pair<command_fe, std::vector<std::string>>> response_data;
	std::vector<std::unique_ptr<accsp_mapped>> response_large_files;

	void set_response(const command_fe key, std::vector<std::string> value)
	{
		std::unique_lock lock(response_mutex);
		if (is_command_overriding(key))
		{
			for (auto& i : response_data)
			{
				if (i.first == key)
				{
					i.second = std::move(value);
					return;
				}
			}
		}
		response_data.emplace_back(key, std::move(value));
	}

	void set_response(const command_fe key, std::string value)
	{
		std::unique_lock lock(response_mutex);
		if (is_command_overriding(key))
		{
			for (auto& i : response_data)
			{
				if (i.first == key)
				{
					i.second.resize(1);
					i.second[0] = std::move(value);
					return;
				}
			}
		}
		response_data.emplace_back(key, std::vector{ std::move(value) });
	}

	static std::mt19937& prng_engine()
	{
		thread_local static std::random_device rd{};
		thread_local static std::mt19937 engine{ rd() };
		return engine;
	}

	static int random_until(const int to_exclusive)
	{
		return std::uniform_int_distribution<>(0, to_exclusive - 1)(prng_engine());
	}

	uint32_t submit_commands()
	{
		auto entry = mmf->entry;
		auto c = 0U, p = 0U;
		auto add_entry_v = [&](command_fe k, const std::vector<std::string>& a, uint64_t total_size)
		{
			if (p + total_size + 4U > ACCSP_FRAME_SIZE) return false;
			*(command_fe*)&entry->response[p] = k;
			*(uint16_t*)&entry->response[p + 1] = uint16_t(total_size);
			p += 3U;
			auto second = false;
			for (const auto& i : a)
			{
				if (second) entry->response[p++] = '\1';
				else second = true;
				memcpy(&entry->response[p], i.data(), i.size() + 1);
				p += uint32_t(i.size());
			}
			++c;
			return true;
		};

		auto add_entry_1 = [&](command_fe k, const std::string& v)
		{
			if (p + v.size() + 4U > ACCSP_FRAME_SIZE) return false;
			*(command_fe*)&entry->response[p] = k;
			*(uint16_t*)&entry->response[p + 1] = uint16_t(v.size());
			memcpy(&entry->response[p + 3U], v.data(), v.size() + 1);
			p += uint32_t(v.size()) + 3U;
			++c;
			return true;
		};

		std::unique_lock lock(response_mutex);
		std::pair<command_fe, std::vector<std::string>>* cut_from{};
		response_large_files.clear();
		for (auto& i : response_data)
		{
			if (i.second.empty()) continue;

			auto total_size = i.second.size() - 1;
			for (auto& v : i.second)
			{
				total_size += v.size();
			}

			if (total_size > ACCSP_MAX_COMMAND_SIZE)
			{
				if (p + 20 > ACCSP_FRAME_SIZE)
				{
					cut_from = &i;
					break;
				}

				const auto key = random_until(INT32_MAX);
				auto name = named_prefix + L'_' + std::to_wstring(key);
				auto item = std::make_unique<accsp_mapped>(name, total_size + 2ULL, false);
				*(command_fe*)item->entry = i.first;
				auto d = (char*)item->entry + 1;
				for (const auto& v : i.second)
				{
					if (d > (char*)item->entry + 1) *d++ = '\1';
					memcpy(d, v.data(), v.size() + 1);
					d += v.size();
				}
				assert(uint64_t(d - ((char*)item->entry + 1)) == total_size);
				std::string o;
				o.resize(8);
				*(int*)o.data() = key;
				*(uint32_t*)(o.data() + 4) = uint32_t(2ULL + total_size);
				add_entry_1(command_fe::large_command, o);
				response_large_files.emplace_back(std::move(item));
			}
			else if (!add_entry_v(i.first, i.second, total_size))
			{
				cut_from = &i;
				break;
			}
		}
		if (cut_from)
		{
			const auto new_size = &*response_data.end() - cut_from;
			memmove(response_data.data(), cut_from, new_size * sizeof(std::pair<command_fe, std::string>));
			response_data.resize(new_size);
		}
		else
		{
			response_data.clear();
		}
		return c;
	}

	void set_reply(std::string reply_id, std::string value)
	{
		if (reply_id.empty()) return;
		std::vector<std::string> v;
		v.emplace_back(std::move(reply_id));
		v.emplace_back(std::move(value));
		set_response(command_fe::reply, std::move(v));
	}

	std::array<accsp_wb_entry::vec2, 2> last_touches{};
	std::chrono::high_resolution_clock::time_point focus_update_time;
	uint32_t fe_alive_last{};
	uint32_t base_flags{};
	float fe_alive_missing{};
	bool last_focus{};
	bool last_hidden{};
	uint8_t visible_counter = 250;

	void update_visible_state()
	{					
		if (const auto browser = safe_browser())
		{	
			if (const auto hidden_now = visible_counter == 0; hidden_now != last_hidden)
			{
				last_hidden = hidden_now;
				button_ctrl = false;
				button_shift = false;
				button_alt = false;
				if (hidden_now)
				{
					CefPostDelayedTask(TID_UI, new BasicTask([that = CefRefPtr(this), browser = CefRefPtr(browser)]
					{
						if (that->last_hidden) 
						{
							browser->GetHost()->WasHidden(true);
						}
					}), 100);
				}
				else
				{
					browser->GetHost()->WasHidden(hidden_now);
				}
				browser->GetHost()->WasResized();
				log_message("Hidden flag: %d (%s)", hidden_now, last_url.c_str());
			}
		}
	}

	void sync()
	{
		auto entry = mmf->entry;
		entry->be_alive_time = time(nullptr);

		if (passthrough_mode_)
		{
			entry->handle = pd_main.current;
			entry->popup_handle = popup_active ? pd_popup.current : 0ULL;
			entry->popup_dimensions = popup_active ? popup_area : std::array<float, 4>{};
		}

		if (const auto browser = safe_browser(); 
			browser && iterate_commands([&](command_be k, const utils::str_view& v)
		{
			control(k, v);
		}))
		{
			entry->commands_set = 0;
		}

		if ((entry->fe_flags & 2) != 0 || entry->needs_next_frame > 0)
		{
			visible_counter = 250;
		}
		else if (visible_counter > 0)
		{
			--visible_counter;
		}

		if (const auto browser = safe_browser())
		{
			if (const auto focus_now = (entry->fe_flags & 1) != 0; focus_now != last_focus 
				|| std::chrono::high_resolution_clock::now() - focus_update_time > std::chrono::milliseconds(1000))
			{
				browser->GetHost()->SetFocus(focus_now);
				last_focus = focus_now;
				focus_update_time = std::chrono::high_resolution_clock::now();
			}

			update_visible_state();

			if (!last_hidden && postponed_scroll.x != INT32_MAX && !last_title.empty() && entry->loading_progress == 65535)
			{
				apply_scroll(postponed_scroll.absolute, postponed_scroll.x, postponed_scroll.y);
				postponed_scroll.x = INT32_MAX;
			}
		}

		const auto mouse_flags = this->last_mouse_flags;
		this->last_mouse_flags = entry->mouse_flags;
		if (last_mouse_x != entry->mouse_x || last_mouse_y != entry->mouse_y)
		{
			mouse_move(entry->mouse_x == UINT16_MAX, entry->mouse_x, entry->mouse_y);
			last_mouse_x = entry->mouse_x;
			last_mouse_y = entry->mouse_y;
		}
		if (entry->mouse_wheel)
		{
			mouse_wheel(entry->mouse_x, entry->mouse_y, 0, entry->mouse_wheel);
		}
		if (mouse_flags != entry->mouse_flags)
		{
			for (auto i = 0; i < 3; ++i)
			{
				const auto m = 1 << i;
				if ((entry->mouse_flags & m) != (mouse_flags & m))
				{
					mouse_click(cef_mouse_button_type_t(i), (entry->mouse_flags & m) == 0, entry->mouse_x, entry->mouse_y);
				}
			}
		}

		for (auto i = 0U; i < last_touches.size(); ++i)
		{
			if (last_touches[i] != entry->touches[i])
			{
				if (std::abs(entry->touches[i].x) < 1e30f)
				{
					touch_event(i, std::abs(last_touches[i].x) < 1e30f ? CEF_TET_MOVED : CEF_TET_PRESSED, entry->touches[i]);
				}
				else if (std::abs(last_touches[i].x) < 1e30f)
				{
					touch_event(i, entry->touches[i].x < 0.f ? CEF_TET_CANCELLED : CEF_TET_RELEASED, last_touches[i]);
				}
				last_touches[i] = entry->touches[i];
			}
		}

		auto flags = base_flags;
		if (const auto browser = safe_browser())
		{
			const auto h = browser->GetHost();
			if (entry->needs_next_frame > 0)
			{
				--entry->needs_next_frame;
				h->SendExternalBeginFrame();
			}
			// if (browser->IsLoading()) flags |= 1;
			// if (browser->CanGoBack()) flags |= 2;
			// if (browser->CanGoForward()) flags |= 4;

			if (browser->HasDocument()) 
			{
				flags |= 8;
				if (own_zoom_phase != _zoom_phase)
				{
					own_zoom_phase = _zoom_phase;
					do_on_ui([that = CefRefPtr(this), host = CefRefPtr(h)]
					{
						that->mmf->entry->zoom_level = host->GetZoomLevel();
					}, true);
				}
			}

			// if (h->IsAudioMuted()) flags |= 16;
			if (fullscreen_active) flags |= 64;

			if (notify_resized > 0)
			{
				--notify_resized;
				// h->WasResized();
			}
		}
		entry->be_flags = flags;

		if (entry->response_set == 0 && !response_data.empty())
		{
			const auto p = submit_commands();
			__faststorefence();
			entry->response_set = p;
		}

		// base::BindOnce();
		// CefTaskRunner::GetForThread(TID_UI)->PostTask()
	}

	// bool OnConsoleMessage(CefRefPtr<CefBrowser> browser, cef_log_severity_t level, const CefString& message, const CefString& source, int line) override;
	// void OnRenderViewReady(CefRefPtr<CefBrowser> browser) override	{}

	void OnResetDialogState(CefRefPtr<CefBrowser> browser) override
	{
		// set_response(command_fe::context_menu)
	}

	float postponed_zoom = FLT_MAX;

	void OnDocumentAvailableInMainFrame(CefRefPtr<CefBrowser> browser) override
	{
		assert(CefCurrentlyOn(TID_UI));
		base_flags |= 8;
		if (postponed_zoom != FLT_MAX)
		{
			browser->GetHost()->SetZoomLevel(postponed_zoom);
			mmf->entry->zoom_level = postponed_zoom;
			postponed_zoom = FLT_MAX;
		}
		else
		{
			mmf->entry->zoom_level = float(browser->GetHost()->GetZoomLevel());
		}

		if (suspended)
		{
			browser->GetMainFrame()->SendProcessMessage(PID_RENDERER, CefProcessMessage::Create(PMSG_KILL));
		}
	}

	void OnLoadingStateChange(CefRefPtr<CefBrowser> browser, bool is_loading, bool can_go_back, bool can_go_forward) override
	{
		base_flags &= ~(1 | 2 | 4);
		if (is_loading) base_flags |= 1;
		if (can_go_back) base_flags |= 2;
		if (can_go_forward) base_flags |= 4;
	}

	std::string last_title;
	std::string last_favicon;
	std::string last_status;
	std::string last_tooltip;

	bool OnCertificateError(CefRefPtr<CefBrowser> browser, cef_errorcode_t cert_error, const CefString& request_url, CefRefPtr<CefSSLInfo> ssl_info,
		CefRefPtr<CefCallback> callback) override
	{
		if (!ignore_certificate_errors_filter._Empty())
		{
			std::unique_lock lock(ignore_certificate_errors_mutex);
			if (test_regex(request_url.ToString(), ignore_certificate_errors_filter))
			{
				callback->Continue();
				return true;
			}
		}
		return false;
	}

	static const char* encode_wodisp(cef_window_open_disposition_t o)
	{
		switch (o)
		{
			case WOD_CURRENT_TAB: return "currentTab";
			case WOD_SINGLETON_TAB: return "singletonTab";
			case WOD_NEW_FOREGROUND_TAB: return "newForegroundTab";
			case WOD_NEW_BACKGROUND_TAB: return "newBackgroundTab";
			case WOD_NEW_POPUP: return "newPopup";
			case WOD_NEW_WINDOW: return "newWindow";
			case WOD_SAVE_TO_DISK: return "saveToDisk";
			case WOD_OFF_THE_RECORD: return "offTheRecord";
			case WOD_IGNORE_ACTION: return "ignoreAction";
			default: return "unknown";
		}
	}

	bool OnOpenURLFromTab(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, const CefString& target_url,
		cef_window_open_disposition_t target_disposition, bool user_gesture) override
	{
		if (redirect_navigation)
		{
			lson_builder b;
			b.add("userGesture", user_gesture);
			b.add("originURL", frame->GetURL().ToString());
			b.add("targetURL", target_url);
			b.add("targetDisposition", encode_wodisp(target_disposition));
			set_response(command_fe::open_url, b.finalize());
			return true;
		}
		return false;
	}

	void OnFaviconURLChange(CefRefPtr<CefBrowser> browser, const std::vector<CefString>& icon_urls) override
	{
		auto str = icon_urls.empty() ? std::string() : icon_urls.front().ToString();
		if (str != last_favicon)
		{
			set_response(command_fe::favicon, str);
			last_favicon = std::move(str);
		}
	}

	void OnAddressChange(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, const CefString& url) override
	{
		if (frame->IsMain())
		{
			auto str = url.ToString();
			if (str != last_url)
			{
				set_response(command_fe::url, str);
				last_url = std::move(str);
			}
		}
	}

	void OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString& title) override
	{
		auto str = title.ToString();
		if (str != last_title)
		{
			set_response(command_fe::title, str);
			last_title = std::move(str);
		}
	}

	void OnStatusMessage(CefRefPtr<CefBrowser> browser, const CefString& value) override
	{
		auto str = value.ToString();
		if (str != last_status)
		{
			set_response(command_fe::status, str);
			last_status = std::move(str);
		}
	}

	bool OnCursorChange(CefRefPtr<CefBrowser> browser, HCURSOR cursor, cef_cursor_type_t type,
		const CefCursorInfo& custom_cursor_info) override
	{
		mmf->entry->cursor = uint8_t(type);
		return true;
	}

	bool OnTooltip(CefRefPtr<CefBrowser> browser, CefString& text) override
	{
		auto str = text.ToString();
		if (str != last_tooltip)
		{
			set_response(command_fe::tooltip, str);
			last_tooltip = std::move(str);
		}
		return true;
	}

	struct download_item
	{
		CefRefPtr<CefDownloadItemCallback> update_callback;
		std::string filename;
		char next_command{};
	};

	std::mutex download_items_mutex;
	std::unordered_map<uint32_t, download_item> download_items;

	void OnBeforeDownload(CefRefPtr<CefBrowser> browser, CefRefPtr<CefDownloadItem> download_item, const CefString& suggested_name,
		CefRefPtr<CefBeforeDownloadCallback> callback) override
	{
		lson_builder b;
		b.add("ID", download_item->GetId());
		b.add("downloadURL", download_item->GetURL());
		b.add("originalURL", download_item->GetOriginalUrl());
		b.add_opt("totalBytes", download_item->GetTotalBytes());
		b.add("mimeType", download_item->GetMimeType());
		b.add_opt("contentDisposition", download_item->GetContentDisposition());
		b.add("suggestedName", suggested_name);
		b.add("replyID", await_reply("OnBeforeDownload", [this, id = download_item->GetId(), callback = std::move(callback)](const utils::str_view& data)
		{
			if (!data.empty())
			{
				auto filename = data.str();
				callback->Continue(filename + ".tmp", false);
				{
					std::unique_lock l(download_items_mutex);
					download_items[id] = {nullptr, std::move(filename), 0};
				}
			}
		}));
		set_response(command_fe::download, b.finalize());
	}

	void OnDownloadUpdated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefDownloadItem> download_item, CefRefPtr<CefDownloadItemCallback> callback) override
	{
		std::unique_lock l(download_items_mutex);
		const auto f = download_items.find(download_item->GetId());
		if (f == download_items.end())
		{
			// callback->Cancel();
			return;
		}

		struct
		{
			uint32_t id;
			uint32_t flags;
			int64 total_bytes;
			int64 current_speed;
			int64 received_bytes;
		} data{};
		data.id = f->first;
		data.total_bytes = download_item->GetTotalBytes();
		data.received_bytes = download_item->GetReceivedBytes();
		data.current_speed = download_item->GetCurrentSpeed();
		if (download_item->IsComplete()) data.flags |= 1;
		if (download_item->IsCanceled()) data.flags |= 2;
		if (download_item->IsInProgress()) data.flags |= 4;
		if ((data.flags & (1 | 2)) != 0 && !f->second.filename.empty())
		{
			const auto tmp_filename = f->second.filename + ".tmp";
			if (data.flags & 1)
			{
				DeleteFileW(utils::utf16(f->second.filename).c_str());
				MoveFileW(utils::utf16(tmp_filename).c_str(), utils::utf16(f->second.filename).c_str());
			}
			else
			{
				DeleteFileW(utils::utf16(tmp_filename).c_str());
			}
			f->second.filename.clear();
		}
		if (f->second.next_command != 0)
		{
			if (f->second.next_command == 'c') callback->Cancel();
			else if (f->second.next_command == 'p') callback->Pause();
			f->second.next_command = 0;
		}
		f->second.update_callback = std::move(callback);
		set_response(command_fe::download_update, std::string((const char*)&data, sizeof data));
	}

	void cancel_download(const std::pair<utils::str_view, utils::str_view>& args)
	{
		std::unique_lock l(download_items_mutex);
		const auto f = download_items.find(args.first.as(0U));
		if (f == download_items.end())
		{
			return;
		}
		if (const auto& c = f->second.update_callback)
		{
			if (args.second == "c") c->Cancel();
			if (args.second == "r") c->Resume();
			if (args.second == "p") c->Pause();
		}
		else if (f->second.next_command != 'c')
		{
			f->second.next_command = args.second[0];
		}
	}

	static const char* encode_wodisp(cef_file_dialog_mode_t o)
	{
		switch (o)
		{
			case FILE_DIALOG_OPEN: return "open";
			case FILE_DIALOG_OPEN_MULTIPLE: return "openMultiple";
			case FILE_DIALOG_OPEN_FOLDER: return "openFolder";
			case FILE_DIALOG_SAVE: return "save";
			default: return "unknown";
		}
	}

	bool OnFileDialog(CefRefPtr<CefBrowser> browser, FileDialogMode mode, const CefString& title, const CefString& default_file_path,
		const std::vector<CefString>& accept_filters, CefRefPtr<CefFileDialogCallback> callback) override
	{
		lson_builder b;
		b.add("type", encode_wodisp(mode));
		b.add_opt("title", title);
		b.add_opt("defaultFilePath", default_file_path);
		b.add("acceptFilters", accept_filters);
		b.add("replyID", await_reply("File dialog", [callback = std::move(callback)](const utils::str_view& data)
		{
			if (data.empty())
			{
				callback->Cancel();
			}
			else
			{
				std::vector<CefString> ret;
				for (const auto& i : data.split('\1', false, false))
				{
					ret.push_back(i);
				}
				callback->Continue(ret);
			}
		}));
		set_response(command_fe::file_dialog, b.finalize());
		return true;
	}

	void resize(int width, int height)
	{
		if (width != width_ || height != height_)
		{
			log_message("WebView:resize(%d, %d): browser=%p", width, height, safe_browser());
			width_ = width;
			height_ = height;
			if (const auto browser = safe_browser())
			{
				browser->GetHost()->WasResized();
			}
			else
			{
				was_resized = true;
			}
		}
	}

	float scale_factor = 1.f;
	bool suspended = false;
	bool was_resized = false;

	bool GetScreenInfo(CefRefPtr<CefBrowser> browser, CefScreenInfo& screen_info) override
	{
		screen_info.device_scale_factor = scale_factor;
		return true;
	}

	cef_event_flags_t get_event_flags()
	{
		int ret{};
		if (last_mouse_flags & 1) ret |= EVENTFLAG_LEFT_MOUSE_BUTTON;
		if (last_mouse_flags & 2) ret |= EVENTFLAG_MIDDLE_MOUSE_BUTTON;
		if (last_mouse_flags & 4) ret |= EVENTFLAG_RIGHT_MOUSE_BUTTON;
		if (button_ctrl) ret |= EVENTFLAG_CONTROL_DOWN;
		if (button_alt) ret |= EVENTFLAG_ALT_DOWN;
		if (button_shift) ret |= EVENTFLAG_SHIFT_DOWN;
		return cef_event_flags_t(ret);
	}

	int32_t last_click_x{};
	int32_t last_click_y{};
	int32_t last_click_count{};
	double last_click_time = -1e9;

	void mouse_click(cef_mouse_button_type_t button, bool up, int32_t x, int32_t y)
	{
		if (const auto browser = safe_browser())
		{
			CefMouseEvent mouse;
			mouse.x = x;
			mouse.y = y;
			mouse.modifiers = get_event_flags();

			auto count = 1;
			if (button != MBT_LEFT)
			{
				last_click_time = -1e9;
			}
			else if (abs(x - last_click_x) > GetSystemMetrics(SM_CXDOUBLECLK) / 2
				|| abs(y - last_click_y) > GetSystemMetrics(SM_CXDOUBLECLK) / 2
				|| uint32_t(time_now_ms() - last_click_time) > GetDoubleClickTime())
			{
				if (!up)
				{
					last_click_time = time_now_ms();
					last_click_x = x;
					last_click_y = y;
				}
				last_click_count = 0;
			}
			else if (last_click_count == 1)
			{
				count = 2;
				if (up) last_click_time = -1e9;
			}
			else if (up)
			{
				last_click_count = 1;
			}

			browser->GetHost()->SendMouseClickEvent(mouse, button, up, count);
		}
	}

	void touch_event(int touch_id, cef_touch_event_type_t type, const accsp_wb_entry::vec2& pos)
	{
		if (const auto browser = safe_browser())
		{
			CefTouchEvent touch;
			touch.pointer_type = CEF_POINTER_TYPE_TOUCH;
			touch.type = type;
			touch.id = touch_id;
			touch.x = pos.x;
			touch.y = pos.y;
			touch.modifiers = get_event_flags();
			browser->GetHost()->SendTouchEvent(touch);
		}
	}

	void mouse_move(bool leave, int32_t x, int32_t y)
	{
		if (const auto browser = safe_browser())
		{
			CefMouseEvent mouse;
			mouse.x = x;
			mouse.y = y;
			mouse.modifiers = get_event_flags();
			browser->GetHost()->SendMouseMoveEvent(mouse, leave);
		}
	}

	void mouse_wheel(int32_t x, int32_t y, int32_t dx, int32_t dy)
	{
		if (const auto browser = safe_browser())
		{
			CefMouseEvent mouse;
			mouse.x = x;
			mouse.y = y;
			mouse.modifiers = get_event_flags();
			browser->GetHost()->SendMouseWheelEvent(mouse, dx, dy);
		}
	}

	CefBrowser* safe_browser()
	{
		return browser_ptr_.load(std::memory_order_relaxed);
	}

private:
	IMPLEMENT_REFCOUNTING(WebView);

	std::atomic<CefBrowser*> browser_ptr_;
	Layer* popup_layer_{};
};

struct WebLayer : Layer
{
	WebLayer(const d3d11::Device& device, CefRefPtr<WebView> view)
		: Layer(device, true), view_(std::move(view))
	{
		log_message("WebLayer(%p, %p)", this, view_.get());
	}

	~WebLayer() override
	{
		log_message("~WebLayer(%p, %p)", this, view_.get());
		view_->close();
	}

	void attach(Composition* comp) override
	{
		Layer::attach(comp);
		view_->attach(comp);
	}

	void resize(int width, int height) override
	{
		const auto rect = bounds();
		view_->resize(int(float(rect.width) * float(width)), int(float(rect.height) * float(height)));
	}

	void render(const d3d11::Context& ctx) override
	{
		render_texture(ctx, view_->texture(ctx));
	}

	void sync() override
	{
		view_->sync();
	}

	void set_handle_prefix(const std::wstring& prefix) override
	{
		view_->named_prefix = prefix;
	}

private:
	const CefRefPtr<WebView> view_;
};

struct CefModule
{
	static void startup(HINSTANCE);
	static void step();
	static void shutdown();
};

static std::shared_ptr<CefModule> CefModule_instance_;
static std::wstring main_data_directory;

void CefModule::startup(HINSTANCE mod)
{
	assert(!CefModule_instance_.get());	
	CefModule_instance_ = std::make_shared<CefModule>();

	CefSettings settings;
	settings.no_sandbox = true;
	settings.windowless_rendering_enabled = true;
	settings.cookieable_schemes_list = utils::str_view("ac");

	_cef_thread = get_env_value(L"ACCSPWB_CEF_THREADING", false);
	if (_cef_thread)
	{
		settings.multi_threaded_message_loop = true;
	}
	else
	{
		settings.multi_threaded_message_loop = false;
		settings.external_message_pump = true;
	}

	wchar_t var_data[256]{};
	if (const auto s = GetEnvironmentVariableW(L"ACCSPWB_USER_AGENT", var_data, 256))
	{
		cef_string_wide_to_utf16(var_data, s, &settings.user_agent);
	}
	if (const auto s = GetEnvironmentVariableW(L"ACCSPWB_ACCEPT_LANGUAGES", var_data, 256))
	{
		cef_string_wide_to_utf16(var_data, s, &settings.accept_language_list);
	}
	if (const auto s = GetEnvironmentVariableW(L"ACCSPWB_DATA_DIRECTORY", var_data, 256))
	{
		main_data_directory = std::wstring(var_data, s);
		cef_string_wide_to_utf16(var_data, s, &settings.cache_path);
	}
	if (const auto s = GetEnvironmentVariableW(L"ACCSPWB_LOG_FILENAME", var_data, 256))
	{
		cef_string_wide_to_utf16(var_data, s, &settings.log_file);
	}
	else
	{
		settings.log_severity = LOGSEVERITY_DISABLE;
	}

	const CefRefPtr app(new WebApp());
	const CefMainArgs main_args(mod);
	CefInitialize(main_args, settings, app, nullptr);
	
	if (!_cef_thread)
	{
		CefDoMessageLoopWork();
	}
}

void CefModule::shutdown()
{
	CefShutdown();
	assert(CefModule_instance_.get());
	if (CefModule_instance_)
	{
		CefModule_instance_.reset();
	}
}

void CefModule::step()
{
	if (_cef_thread) return;
	CefDoMessageLoopWork();
}

std::shared_ptr<Layer> create_web_layer(std::shared_ptr<accsp_mapped_typed<accsp_wb_entry>> entry, const d3d11::Device& device, bool* passthrough_mode_out, bool has_full_access)
{
	auto passthrough_mode = true;
	auto redirect_audio = false;
	auto dev_tools = 0;
	auto uuid = 0;
	CefPoint inspect_at{};

	CefWindowInfo window_info;
	window_info.SetAsWindowless(nullptr);
	window_info.shared_texture_enabled = true;
	window_info.external_begin_frame_enabled = true;

	CefBrowserSettings settings;
	settings.chrome_status_bubble = STATE_DISABLED;
	settings.windowless_frame_rate = 60;

	CefRequestContextSettings context;
	context.cookieable_schemes_list = utils::str_view("ac");

	for (auto line : utils::str_view((*entry)->response).split('\n', true, true))
	{
		auto kv = line.pair('=');
		if (kv.first == "UUID") uuid = kv.second.as(0);
		if (kv.first == "directRender") passthrough_mode = kv.second.as(0) != 0;
		if (kv.first == "redirectAudio") redirect_audio = kv.second.as(0) != 0;
		if (kv.first == "devTools") dev_tools = kv.second.as(0);
		if (kv.first == "devToolsInspect") inspect_at = CefPoint{kv.second.pair(',').first.as(0), kv.second.pair(',').second.as(0)};
		if (kv.first == "backgroundColor") settings.background_color = kv.second.as(0U);
		if (kv.first == "standardFontFamily") settings.standard_font_family = kv.second;
		if (kv.first == "sansSerifFontFamily") settings.sans_serif_font_family = kv.second;
		if (kv.first == "serifFontFamily") settings.serif_font_family = kv.second;
		if (kv.first == "cursiveFontFamily") settings.cursive_font_family = kv.second;
		if (kv.first == "fantasyFontFamily") settings.fantasy_font_family = kv.second;
		if (kv.first == "fixedFontFamily") settings.fixed_font_family = kv.second;
		if (kv.first == "minimumFontSize") settings.minimum_font_size = kv.second.as(0);
		if (kv.first == "minimumLogicalFontSize") settings.minimum_logical_font_size = kv.second.as(0);
		if (kv.first == "defaultFontSize") settings.default_font_size = kv.second.as(0);
		if (kv.first == "defaultFixedFontSize") settings.default_fixed_font_size = kv.second.as(0);
		if (kv.first == "defaultEncoding") settings.default_encoding = kv.second;
		if (kv.first == "acceptLanguages") context.accept_language_list = kv.second;

		if (kv.first == "imageLoading") settings.image_loading = kv.second.as(0U) ? STATE_ENABLED : STATE_DISABLED;
		if (kv.first == "javascript") settings.javascript = kv.second.as(0U) ? STATE_ENABLED : STATE_DISABLED;
		if (kv.first == "remoteFonts") settings.remote_fonts = kv.second.as(0U) ? STATE_ENABLED : STATE_DISABLED;
		if (kv.first == "localStorage") settings.local_storage = kv.second.as(0U) ? STATE_ENABLED : STATE_DISABLED;
		if (kv.first == "databases") settings.databases = kv.second.as(0U) ? STATE_ENABLED : STATE_DISABLED;
		if (kv.first == "webGL") settings.webgl = kv.second.as(0U) ? STATE_ENABLED : STATE_DISABLED;
		if (kv.first == "shrinkImagesToFit") settings.image_shrink_standalone_to_fit = kv.second.as(0U) ? STATE_ENABLED : STATE_DISABLED;
		if (kv.first == "textAreaResize") settings.text_area_resize = kv.second.as(0U) ? STATE_ENABLED : STATE_DISABLED;
		if (kv.first == "tabToLinks") settings.tab_to_links = kv.second.as(0U) ? STATE_ENABLED : STATE_DISABLED;

		if (kv.first == "dataKey")
		{
			const auto path = kv.second.empty() ? main_data_directory : main_data_directory + L"\\" + utf16(kv.second);
			cef_string_wide_to_utf16(path.data(), path.size(), &context.cache_path);
		}
	}

	assert(passthrough_mode_out);
	if (passthrough_mode_out)
	{
		*passthrough_mode_out = passthrough_mode;
	}

	CefRefPtr view(new WebView(std::move(entry), device, passthrough_mode, redirect_audio, uuid, has_full_access));
	if (dev_tools)
	{
		CefRefPtr<WebView> parent;
		{
			std::unique_lock lock(_alive_mutex);
			if (auto found = _alive_instances.find(dev_tools); found != _alive_instances.end())
			{
				parent = found->second;
			}
		}
		if (parent && parent->safe_browser())
		{
			parent->safe_browser()->GetHost()->ShowDevTools(window_info, view, settings, inspect_at);
		}
		else
		{
			CefBrowserHost::CreateBrowser(window_info, view, "about:blank#blocked", settings, nullptr, nullptr);
		}
	}
	else
	{
		auto ctx(CefRequestContext::CreateContext(context, nullptr));
		ctx->RegisterSchemeHandlerFactory("ac", "", view);
		CefBrowserHost::CreateBrowser(window_info, view, view->initial_url, settings, nullptr, std::move(ctx));
	}
	return std::make_shared<WebLayer>(device, view);
}

int cef_initialize(HINSTANCE instance)
{
	if (get_env_value(L"ACCSPWB_HIGH_DPI_SUPPORT", false))
	{
		CefEnableHighDPISupport();
	}

	{
		const CefRefPtr app(new WebApp());
		const CefMainArgs main_args(instance);		
		if (const auto exit_code = CefExecuteProcess(main_args, app, nullptr); exit_code >= 0) return exit_code;
	}

	CefModule::startup(instance);
	return -1;
}

void cef_step()
{
	CefModule::step();
}

void cef_uninitialize()
{
	CefModule::shutdown();
}
