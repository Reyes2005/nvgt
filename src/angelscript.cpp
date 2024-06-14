/* angelscript.cpp - Angelscript integration code
 * This contains much of the code that revolves around Angelscript specifically. Random amounts of this code were initially taken from the Angelscript asrun and asbuild samples before being heavily modified for our purposes.
 * I wrote much of this at the beginning of nvgt's development and haven't touched it enough since, such that parts of this could definetly be cleaner.
 *
 * NVGT - NonVisual Gaming Toolkit
 * Copyright (c) 2022-2024 Sam Tupy
 * https://nvgt.gg
 * This software is provided "as-is", without any express or implied warranty. In no event will the authors be held liable for any damages arising from the use of this software.
 * Permission is granted to anyone to use this software for any purpose, including commercial applications, and to alter it and redistribute it freely, subject to the following restrictions:
 * 1. The origin of this software must not be misrepresented; you must not claim that you wrote the original software. If you use this software in a product, an acknowledgment in the product documentation would be appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
*/

#define NOMINMAX
#include "UI.h"
#include "network.h"
#include <exception>
#include <string>
#include <angelscript.h>
#include <Poco/DateTime.h>
#include <Poco/Environment.h>
#include <Poco/Exception.h>
#include <Poco/File.h>
#include <Poco/FileStream.h>
#include <Poco/Glob.h>
#include <Poco/Mutex.h>
#include <Poco/Path.h>
#include <Poco/UnbufferedStreamBuf.h>
#include <Poco/zlib.h>
#include <Poco/Util/Application.h>
#include <SDL2/SDL.h>
#include "angelscript.h"
#include "bullet3.h"
#include "compression.h"
#include "crypto.h"
#include "datastreams.h"
#include "hash.h"
#include "input.h"
#include "internet.h"
#include "library.h"
#include "map.h"
#include "misc_functions.h"
#include "nvgt.h"
#ifndef NVGT_USER_CONFIG
	#include "nvgt_config.h"
#else
	#include "../user/nvgt_config.h"
#endif
#include "nvgt_plugin.h"
#include "pack.h"
#include "pathfinder.h"
#include "pocostuff.h"
#include "random.h"
#include "scriptstuff.h"
#include "serialize.h"
#include "sound.h"
#include "srspeech.h"
#include "system_fingerprint.h"
#include "threading.h"
#include "timestuff.h"
#include "tts.h"
#include "version.h"

#ifndef NVGT_STUB
	#include "scriptbuilder.h"
	#include "debugger.h"
#endif
#include "scriptstdstring.h"
#include "print_func.h"
#include "scriptany.h"
#include "scriptarray.h"
#include "scriptdictionary.h"
#include "filesystem.h"
#include "scriptgrid.h"
#include "scripthandle.h"
#include "scripthelper.h"
#include "scriptmath.h"
#include "scriptmathcomplex.h"
#include "contextmgr.h"
#include "weakref.h"
#ifndef NVGT_STUB
	int PragmaCallback(const std::string& pragmaText, CScriptBuilder& builder, void* /*userParam*/);
#endif
asIScriptContext* RequestContextCallback(asIScriptEngine* engine, void* /*param*/);
void ReturnContextCallback(asIScriptEngine* engine, asIScriptContext* ctx, void* /*param*/);
void ExceptionHandlerCallback(asIScriptContext* ctx, void* obj);

using namespace std;
using namespace Poco;

CContextMgr* g_ctxMgr = nullptr;
#ifndef NVGT_STUB
	CDebugger* g_dbg = nullptr;
#endif
int g_bcCompressionLevel = 9;
string g_last_exception_callstack;
string g_compiled_basename;
vector<asIScriptContext*> g_ctxPool;
Mutex g_ctxPoolMutex;
vector<string> g_IncludeDirs;
vector<string> g_IncludeScripts;
std::string g_CommandLine;
CScriptArray* g_command_line_args = 0;
bool g_debug = true; // Whether script has been compiled with extra debug information in the bytecode, true by default because source runs contain such information.
bool g_ASDebugBreak = false; // If the angelscript debugger is in use, user can ctrl+c to perform a manual break.
asIScriptEngine* g_ScriptEngine = NULL;
std::string g_command_line;
int g_LastError;
int g_retcode = 0;
bool g_initialising_globals = true;
bool g_shutting_down = false;
std::string g_stub = "";
std::string g_platform = "auto";
bool g_make_console = false;

class NVGTBytecodeStream : public asIBinaryStream {
	unsigned char* content;
	z_stream zstr;
	int cursor;
	int written_size;
	int alloc_size;
	const int buffer_size = 32 * 1024;
public:
	NVGTBytecodeStream() {
		content = NULL;
		memset(&zstr, 0, sizeof(z_stream));
		zstr.data_type = -1;
		cursor = 0;
		written_size = 0;
		alloc_size = 0;
	}
	~NVGTBytecodeStream() {
		if (zstr.data_type == -1) return;
		#ifndef NVGT_STUB
		deflateEnd(&zstr);
		#else
		inflateEnd(&zstr);
		#endif
	}
	#ifndef NVGT_STUB
	int Write(const void* ptr, asUINT size) {
		if (!content) {
			content = (unsigned char*)malloc(buffer_size);
			zstr.data_type = 0;
			deflateInit(&zstr, g_bcCompressionLevel);
			zstr.next_out = (Bytef*)content;
			cursor = 0;
			written_size = 0;
			alloc_size = buffer_size;
			zstr.avail_out = alloc_size;
		}
		written_size += size;
		if (written_size > alloc_size) {
			alloc_size *= 2;
			content = (unsigned char*)realloc(content, alloc_size);
			zstr.next_out = (Bytef*)(content + zstr.total_out);
			zstr.avail_out = alloc_size - zstr.total_out;
		}
		zstr.next_in = (Bytef*)ptr;
		zstr.avail_in = size;
		while (zstr.avail_in > 0) deflate(&zstr, Z_NO_FLUSH);
		cursor += size;
		return size;
	}
	#else
	int Write(const void* ptr, asUINT size) {
		return -1;
	}
	#endif
	int Read(void* ptr, asUINT size) {
		zstr.next_out = (Bytef*)ptr;
		zstr.avail_out = size;
		inflate(&zstr, Z_SYNC_FLUSH);
		cursor += size;
		return size;
	}
	void reset_cursor() {
		cursor = 0; // This storage area holds more than bytecode, and after extra non-bytecode data is read, we may need to reset the variable keeping track of the number of bytes read encase we need to use that information later for debugging angelscript bytecode load failures which only provide an offset of bytes read in the stream as debug info. We don't store our non-bytecode data at the end of the stream to avoid any imagined edgecase where Angelscript could read a few less bytes than it's written during compilation thus making such data inaccessible.
	}
	// Receives raw bytes read from a compiled executable for decryption and decompression.
	void set(unsigned char* code, int size) {
		written_size = angelscript_bytecode_decrypt(code, size, alloc_size);
		content = code;
		zstr.data_type = 0;
		inflateInit(&zstr);
		zstr.next_in = (Bytef*)content;
		zstr.avail_in = written_size;
	}
	#ifndef NVGT_STUB
	// ZLib compress and encrypt the bytecode for saving to a compiled binary. Encryption is handled by function angelscript_bytecode_encrypt in nvgt_config.h. If that function needs to change the size of the data, it should realloc() the data.
	int get(unsigned char** code) {
		if (zstr.avail_out < buffer_size) {
			alloc_size += buffer_size;
			zstr.avail_out += buffer_size;
			content = (unsigned char*)realloc(content, alloc_size);
		}
		zstr.next_out = (Bytef*)(content + zstr.total_out);
		deflate(&zstr, Z_FINISH);
		written_size = zstr.total_out;
		written_size = angelscript_bytecode_encrypt(content, written_size, alloc_size);
		*code = content;
		return written_size;
	}
	#endif
};
// Since we will likely want to store more in this encrypted data section than just bytecode, create a c++ iostream around the above class. If this had been written when I had more experience, the above class would be written around an existing iostream instead and indeed this is likely to happen at some point.
class nvgt_bytecode_stream_iostream_buf : public UnbufferedStreamBuf {
public:
	nvgt_bytecode_stream_iostream_buf(NVGTBytecodeStream* stream) : stream(stream) {}
private:
	int readFromDevice() {
		char c;
		if (stream->Read(&c, 1) != 1) return char_traits::eof();
		return c;
	}
	int writeToDevice(char c) { return stream->Write(&c, 1); }
	NVGTBytecodeStream* stream;
};
class nvgt_bytecode_stream_ios : public virtual std::ios {
public:
	nvgt_bytecode_stream_ios(NVGTBytecodeStream* stream) : _buf(stream) { poco_ios_init(&_buf); }
	nvgt_bytecode_stream_iostream_buf* rdbuf() { return &_buf; }
protected:
	nvgt_bytecode_stream_iostream_buf _buf;
};
class nvgt_bytecode_istream : public nvgt_bytecode_stream_ios, public std::istream {
public:
	nvgt_bytecode_istream(NVGTBytecodeStream* stream) : nvgt_bytecode_stream_ios(stream), std::istream(&_buf) {}
};
class nvgt_bytecode_ostream : public nvgt_bytecode_stream_ios, public std::ostream {
public:
	nvgt_bytecode_ostream(NVGTBytecodeStream* stream) : nvgt_bytecode_stream_ios(stream), std::ostream(&_buf) {}
};


string g_scriptMessagesWarn;
string g_scriptMessagesErr;
string g_scriptMessagesLine0;
string g_scriptMessagesInfo;
int g_scriptMessagesErrNum;
void ShowAngelscriptMessages() {
	if (g_scriptMessagesErr == "" && g_scriptMessagesWarn == "" && g_scriptMessagesLine0 == "") return;
	#ifdef _WIN32
	if (Util::Application::instance().config().hasOption("application.gui")) {
		if (g_scriptMessagesErrNum)
			info_box("Compilation error", "", (g_ScriptEngine->GetEngineProperty(asEP_COMPILER_WARNINGS) == 2? g_scriptMessagesWarn : "") + (g_scriptMessagesErr != "" ? g_scriptMessagesErr : g_scriptMessagesLine0));
		else
			info_box("Compilation warnings", "", g_scriptMessagesWarn);
	} else {
	#endif
		if (g_scriptMessagesErrNum)
			message((g_ScriptEngine->GetEngineProperty(asEP_COMPILER_WARNINGS) == 2? g_scriptMessagesWarn : "") + (g_scriptMessagesErr != "" ? g_scriptMessagesErr : g_scriptMessagesLine0), "Compilation error");
		else
			message(g_scriptMessagesWarn, "Compilation warnings");
		#ifdef _WIN32
	} // endif gui
		#endif
		g_scriptMessagesErr = g_scriptMessagesWarn = g_scriptMessagesLine0 = ""; // Clear out the message buffers such that only new messages will be displayed upon a second call to this function.
}

void MessageCallback(const asSMessageInfo* msg, void* param) {
	const char* type = "ERROR";
	if (msg->type == asMSGTYPE_WARNING)
		type = "WARNING";
	else if (msg->type == asMSGTYPE_INFORMATION)
		type = "INFO";
	else
		g_scriptMessagesErrNum += 1;
	char buffer[8192];
	snprintf(buffer, 8192, "file: %s\r\nline: %d (%d)\r\n%s: %s\r\n\r\n", msg->section, msg->row > 0 ? msg->row : 0, msg->col > 0 ? msg->col : 0, type, msg->message);
	if (msg->type == asMSGTYPE_INFORMATION)
		g_scriptMessagesInfo = buffer;
	else if (msg->type == asMSGTYPE_ERROR) {
		if (msg->row != 0)
			g_scriptMessagesErr += g_scriptMessagesInfo + buffer;
		else
			g_scriptMessagesLine0 += g_scriptMessagesInfo + buffer;
	} else
		g_scriptMessagesWarn += g_scriptMessagesInfo + buffer;
}
void nvgt_line_callback(asIScriptContext* ctx, void* obj) {
	#ifndef NVGT_STUB
	if (g_dbg) {
		if (g_ASDebugBreak) {
			g_ASDebugBreak = false;
			cout << "user debug break" << endl;
			g_dbg->TakeCommands(ctx);
		}
		g_dbg->LineCallback(ctx);
	}
	#endif
	profiler_callback(ctx, obj);
}
#ifndef NVGT_STUB
int IncludeCallback(const char* filename, const char* sectionname, CScriptBuilder* builder, void* param) {
	File include_file;
	try {
		Path include(Path::expand(filename));
		include.makeAbsolute();
		if (include.getExtension() != "nvgt") { // Embedded pack
			embed_pack(include.toString(), filename);
			return 0;
		}
		include_file = include;
		if (include_file.exists() && include_file.isFile()) return builder->AddSectionFromFile(include.toString().c_str());
		include = Path(sectionname).parent().append(filename);
		include_file = include;
		if (include_file.exists() && include_file.isFile()) return builder->AddSectionFromFile(include.toString().c_str());
		for (int i = 0; i < g_IncludeDirs.size(); i++) {
			include = Path(g_IncludeDirs[i]).append(filename);
			include_file = include;
			if (include_file.exists() && include_file.isFile()) return builder->AddSectionFromFile(include.toString().c_str());
		}
	} catch (Exception& e) {} // Might be wildcards.
	try {
		set<string> includes;
		Glob::glob(Path(sectionname).parent().append(filename), includes, Glob::GLOB_DOT_SPECIAL | Glob::GLOB_FOLLOW_SYMLINKS | Glob::GLOB_CASELESS);
		if (includes.size() == 0) Glob::glob(Path(filename).makeAbsolute(), includes, Glob::GLOB_DOT_SPECIAL | Glob::GLOB_FOLLOW_SYMLINKS | Glob::GLOB_CASELESS);
		for (int i = 0; i < g_IncludeDirs.size(); i++) {
			if (includes.size() == 0) Glob::glob(Path(g_IncludeDirs[i]).append(filename), includes, Glob::GLOB_DOT_SPECIAL | Glob::GLOB_FOLLOW_SYMLINKS | Glob::GLOB_CASELESS);
		}
		for (const std::string& i : includes) {
			include_file = i;
			if (include_file.exists() && include_file.isFile()) builder->AddSectionFromFile(i.c_str());
		}
		if (includes.size() > 0) return 1; // So that the below failure message won't execute.
	} catch (Exception& e) {
		message(e.displayText().c_str(), "exception while finding includes");
	}
	builder->GetEngine()->WriteMessage(filename, 0, 0, asMSGTYPE_ERROR, "unable to locate this include");
	return -1;
}
#endif
void TranslateException(asIScriptContext* ctx, void* /*userParam*/) {
	try {
		throw;
	} catch (Exception& e) {
		ctx->SetException(e.displayText().c_str());
	} catch (std::exception& e) {
		ctx->SetException(e.what());
	} catch (...) {}
}
void Exit(int retcode = 0) {
	g_shutting_down = true;
	g_retcode = retcode;
	g_ctxMgr->AbortAll();
}
asUINT GetTimeCallback() {
	return ticks();
}

int ConfigureEngine(asIScriptEngine* engine) {
	engine->SetMessageCallback(asFUNCTION(MessageCallback), 0, asCALL_CDECL);
	engine->SetTranslateAppExceptionCallback(asFUNCTION(TranslateException), 0, asCALL_CDECL);
	engine->SetEngineProperty(asEP_ALLOW_UNSAFE_REFERENCES, true);
	engine->SetEngineProperty(asEP_INIT_GLOBAL_VARS_AFTER_BUILD, false);
	engine->SetDefaultAccessMask(NVGT_SUBSYSTEM_GENERAL);
	RegisterStdString(engine);
	RegisterScriptAny(engine);
	RegisterScriptArray(engine, true);
	RegisterStdStringUtils(engine);
	RegisterScriptDictionary(engine);
	engine->SetDefaultAccessMask(NVGT_SUBSYSTEM_DATETIME);
	engine->SetDefaultAccessMask(NVGT_SUBSYSTEM_GENERAL);
	RegisterScriptGrid(engine);
	RegisterScriptHandle(engine);
	RegisterScriptMath(engine);
	RegisterScriptMathComplex(engine);
	RegisterScriptWeakRef(engine);
	engine->SetDefaultAccessMask(NVGT_SUBSYSTEM_TERMINAL);
	Print::asRegister(engine);
	engine->SetDefaultAccessMask(NVGT_SUBSYSTEM_GENERAL);
	RegisterExceptionRoutines(engine);
	engine->RegisterGlobalProperty("const string last_exception_call_stack", &g_last_exception_callstack);
	RegisterScriptBullet3(engine);
	engine->SetDefaultAccessMask(NVGT_SUBSYSTEM_DATA);
	RegisterScriptCompression(engine);
	RegisterScriptCrypto(engine);
	RegisterScriptDatastreams(engine);
	engine->SetDefaultAccessMask(NVGT_SUBSYSTEM_DATA);
	RegisterScriptHash(engine);
	engine->SetDefaultAccessMask(NVGT_SUBSYSTEM_INPUT);
	RegisterInput(engine);
	RegisterInternet(engine);
	RegisterScriptLibrary(engine);
	RegisterScriptMap(engine);
	RegisterMiscFunctions(engine);
	engine->SetDefaultAccessMask(NVGT_SUBSYSTEM_NET);
	RegisterScriptNetwork(engine);
	engine->SetDefaultAccessMask(NVGT_SUBSYSTEM_SPEECH);
	RegisterScreenReaderSpeech(engine);
	engine->SetDefaultAccessMask(NVGT_SUBSYSTEM_FS);
	RegisterScriptPack(engine);
	engine->SetDefaultAccessMask(NVGT_SUBSYSTEM_PATHFINDER);
	RegisterScriptPathfinder(engine);
	engine->SetDefaultAccessMask(NVGT_SUBSYSTEM_GENERAL);
	RegisterPocostuff(engine);
	RegisterScriptRandom(engine);
	RegisterScriptstuff(engine);
	engine->SetDefaultAccessMask(NVGT_SUBSYSTEM_OS);
	engine->SetDefaultAccessMask(NVGT_SUBSYSTEM_GENERAL);
	RegisterSerializationFunctions(engine);
	engine->SetDefaultAccessMask(NVGT_SUBSYSTEM_SOUND);
	RegisterScriptSound(engine);
	engine->SetDefaultAccessMask(NVGT_SUBSYSTEM_UNCLASSIFIED);
	RegisterSystemFingerprintFunction(engine);
	engine->SetDefaultAccessMask(NVGT_SUBSYSTEM_OS);
	engine->RegisterGlobalFunction("void exit(int=0)", asFUNCTION(Exit), asCALL_CDECL);
	RegisterThreading(engine);
	RegisterScriptTimestuff(engine);
	engine->SetDefaultAccessMask(NVGT_SUBSYSTEM_FS);
	RegisterScriptFileSystemFunctions(engine);
	engine->SetDefaultAccessMask(NVGT_SUBSYSTEM_SPEECH);
	RegisterTTSVoice(engine);
	RegisterUI(engine);
	g_ctxMgr = new CContextMgr();
	g_ctxMgr->SetGetTimeCallback(GetTimeCallback);
	RegisterUnsorted(engine);
	engine->SetDefaultAccessMask(NVGT_SUBSYSTEM_UNCLASSIFIED);
	g_ctxMgr->RegisterThreadSupport(engine);
	g_ctxMgr->RegisterCoRoutineSupport(engine);
	engine->SetContextCallbacks(RequestContextCallback, ReturnContextCallback, 0);
	engine->SetDefaultAccessMask(NVGT_SUBSYSTEM_GENERAL);
	return 0;
}
#ifndef NVGT_STUB
// The following function translates various configuration options into Angelscript engine properties.
void ConfigureEngineOptions(asIScriptEngine* engine) {
	Util::LayeredConfiguration& config = Util::Application::instance().config();
	if (config.hasOption("scripting.allow_multiline_strings")) engine->SetEngineProperty(asEP_ALLOW_MULTILINE_STRINGS, true);
	if (config.hasOption("scripting.allow_UNICODE_IDENTIFIERS")) engine->SetEngineProperty(asEP_ALLOW_UNICODE_IDENTIFIERS, true);
	if (config.hasOption("scripting.allow_implicit_handle_types")) engine->SetEngineProperty(asEP_ALLOW_IMPLICIT_HANDLE_TYPES, true);
	if (config.hasOption("scripting.disallow_empty_list_elements")) engine->SetEngineProperty(asEP_DISALLOW_EMPTY_LIST_ELEMENTS, true);
	if (config.hasOption("scripting.disallow_global_vars")) engine->SetEngineProperty(asEP_DISALLOW_GLOBAL_VARS, true);
	if (config.hasOption("scripting.disallow_value_assign_for_ref_type")) engine->SetEngineProperty(asEP_DISALLOW_VALUE_ASSIGN_FOR_REF_TYPE, true);
	if (config.hasOption("scripting.disable_integer_division")) engine->SetEngineProperty(asEP_DISABLE_INTEGER_DIVISION, true);
	if (config.hasOption("scripting.use_character_literals")) engine->SetEngineProperty(asEP_USE_CHARACTER_LITERALS, true);
	if (config.hasOption("scripting.ignore_duplicate_shared_interface")) engine->SetEngineProperty(asEP_IGNORE_DUPLICATE_SHARED_INTF, true);
	if (config.hasOption("scripting.private_prop_as_protected")) engine->SetEngineProperty(asEP_PRIVATE_PROP_AS_PROTECTED, true);
	if (config.hasOption("scripting.always_impl_default_construct")) engine->SetEngineProperty(asEP_ALWAYS_IMPL_DEFAULT_CONSTRUCT, true);
	if (config.hasOption("scripting.expand_default_array_to_template")) engine->SetEngineProperty(asEP_EXPAND_DEF_ARRAY_TO_TMPL, true);
	if (config.hasOption("scripting.require_enum_scope")) engine->SetEngineProperty(asEP_REQUIRE_ENUM_SCOPE, true);
	if (config.hasOption("scripting.do_not_optimize_bytecode")) engine->SetEngineProperty(asEP_OPTIMIZE_BYTECODE, false);
	engine->SetEngineProperty(asEP_MAX_NESTED_CALLS, config.getInt("scripting.max_nested_calls", 10000));
	engine->SetEngineProperty(asEP_MAX_STACK_SIZE, config.getInt("scripting.max_stack_size", 0));
	engine->SetEngineProperty(asEP_MAX_CALL_STACK_SIZE, config.getInt("scripting.max_call_stack_size", 0));
	engine->SetEngineProperty(asEP_INIT_STACK_SIZE, config.getInt("scripting.init_stack_size", 4096));
	engine->SetEngineProperty(asEP_INIT_CALL_STACK_SIZE, config.getInt("scripting.init_call_stack_size", 10));
	engine->SetEngineProperty(asEP_PROPERTY_ACCESSOR_MODE, config.getInt("scripting.property_accessor_mode", 3));
	engine->SetEngineProperty(asEP_COMPILER_WARNINGS, config.getInt("scripting.compiler_warnings", 0)); // We must disable these by default for the sake of the megabytes of bgt code that exists.
	engine->SetEngineProperty(asEP_HEREDOC_TRIM_MODE, config.getInt("scripting.heredoc_trim_mode", 1));
	engine->SetEngineProperty(asEP_ALTER_SYNTAX_NAMED_ARGS, config.getInt("scripting.alter_syntax_named_args", 2));
}
int CompileScript(asIScriptEngine* engine, const string& scriptFile) {
	Path global_include(Path(Path::self()).parent().append("include"));
	g_IncludeDirs.push_back(global_include.toString());
	if (!g_debug)
		engine->SetEngineProperty(asEP_BUILD_WITHOUT_LINE_CUES, true);
	CScriptBuilder builder;
	builder.SetIncludeCallback(IncludeCallback, 0);
	builder.SetPragmaCallback(PragmaCallback, 0);
	if (builder.StartNewModule(engine, "nvgt_game") < 0)
		return -1;
	asIScriptModule* mod = builder.GetModule();
	if (mod) mod->SetAccessMask(NVGT_SUBSYSTEM_EVERYTHING);
	if (builder.AddSectionFromFile(scriptFile.c_str()) < 0)
		return -1;
	for (unsigned int i = 0; i < g_IncludeScripts.size(); i++) {
		if (builder.AddSectionFromFile(g_IncludeScripts[i].c_str()) < 0)
			return -1;
	}
	if (builder.BuildModule() < 0) {
		engine->WriteMessage(scriptFile.c_str(), 0, 0, asMSGTYPE_ERROR, "Script failed to build");
		return -1;
	}
	// Do not let the script compile if it contains no entry point.
	if (!mod) return -1;
	asIScriptFunction* func = mod->GetFunctionByDecl("int main()");
	if (!func)
		func = mod->GetFunctionByDecl("void main()");
	if (!func) {
		g_scriptMessagesInfo = "";
		engine->WriteMessage(scriptFile.c_str(), 0, 0, asMSGTYPE_ERROR, "No entry point found (either 'int main()' or 'void main()'.)");
		return -1;
	}
	return 0;
}
int SaveCompiledScript(asIScriptEngine* engine, unsigned char** output) {
	asIScriptModule* mod = engine->GetModule("nvgt_game", asGM_ONLY_IF_EXISTS);
	if (mod == 0)
		return -1;
	NVGTBytecodeStream codestream;
	nvgt_bytecode_ostream ostr(&codestream);
	BinaryWriter bw(ostr);
	serialize_nvgt_plugins(bw);
	for(int i = 0; i < asEP_LAST_PROPERTY; i++) bw.write7BitEncoded(UInt64(engine->GetEngineProperty(asEEngineProp(i))));
	if (mod->SaveByteCode(&codestream, !g_debug) < 0)
		return -1;
	return codestream.get(output);
}
int CompileExecutable(asIScriptEngine* engine, const string& scriptFile) {
	#ifdef _WIN32
	if (g_platform == "auto") g_platform = "windows";
	#elif defined(__linux__)
	if (g_platform == "auto") g_platform = "linux";
	#elif defined(__APPLE__)
	if (g_platform == "auto") g_platform = "mac"; // Todo: detect difference between IOS and macos (need to look up the correct macros).
	#else
	return -1;
	#endif
	Path stubspath = Util::Application::instance().config().getString("application.dir");
	#ifdef __APPLE__ // Stub may be in Resources directory of an app bundle.
	if (!File(Path(stubspath).pushDirectory("stub")).exists() && stubspath[stubspath.depth() -1] == "MacOS" && stubspath[stubspath.depth() -2] == "Contents") stubspath.makeParent().pushDirectory("Resources");
	#endif
	stubspath.pushDirectory("stub");
	Poco::File stub = format("%snvgt_%s%s.bin", stubspath.toString(), g_platform, (g_stub != "" ? string("_") + g_stub : ""));
	Path outpath(!g_compiled_basename.empty() ? g_compiled_basename : Path(scriptFile).setExtension(""));
	if (g_platform == "windows") outpath.setExtension("exe");
	try {
		stub.copyTo(outpath.toString());
		File(outpath).setExecutable();
	} catch (Exception& e) {
		g_ScriptEngine->WriteMessage(scriptFile.c_str(), 0, 0, asMSGTYPE_ERROR, format("failed to copy %s to %s, %s", stub.path(), outpath.toString(), e.displayText()).c_str());
		return -1;
	}
	FileStream fs;
	try {
		fs.open(outpath.toString(), std::ios::in | std::ios::out | std::ios::ate);
		BinaryReader br(fs);
		BinaryWriter bw(fs);
		UInt64 stub_size = fs.size();
		if (g_platform == "windows") {
			// NVGT distributes windows stubs with the first 2 bytes of the PE header modified so that they are not recognised as executables, this avoids an extra AV scan when the stub is copied which may add a few hundred ms to compile times. Fix them now in the copied file.
			fs.seekp(0);
			bw.writeRaw("MZ");
			if (g_make_console) { // The user wants to compile their app without /subsystem:windows
				int subsystem_offset;
				fs.seekg(60); // position of new PE header address.
				br >> subsystem_offset;
				subsystem_offset += 92; // offset in new PE header containing subsystem word. 2 for GUI, 3 for console.
				fs.seekp(subsystem_offset);
				bw << UInt16(3);
			}
		}
		// Other code that does platform specific things can go here, for now the platforms we support do nearly the same from now on.
		fs.seekp(0, std::ios::end);
		write_embedded_packs(bw);
		unsigned char* code = NULL;
		UInt32 code_size = SaveCompiledScript(engine, &code);
		if (code_size < 1) {
			engine->WriteMessage(scriptFile.c_str(), 0, 0, asMSGTYPE_ERROR, format("failed to retrieve bytecode while trying to compile %s", outpath.toString()).c_str());
			return -1;
		}
		bw.write7BitEncoded(code_size ^ NVGT_BYTECODE_NUMBER_XOR);
		bw.writeRaw((const char*)code, code_size);
		if (g_platform != "windows") bw << int(stub_size); // All platforms but windows currently read our data offset from the end of the executable, but this is likely subject to change as we learn about any negative consequences of doing so on various platforms.
		fs.close(); // Compilation success!
		free(code);
		bool quiet = Util::Application::instance().config().hasOption("application.quiet") || Util::Application::instance().config().hasOption("application.QUIET"); // Maybe we should switch to a verbocity level?
		if (quiet) return 0;
		message(format("%s build succeeded in %?ums, saved to %s", string(g_debug ? "Debug" : "Release"), Util::Application::instance().uptime().totalMilliseconds(), outpath.toString()), "Success!");
	} catch (Exception& e) {
		engine->WriteMessage(scriptFile.c_str(), 0, 0, asMSGTYPE_ERROR, format("failed to compile %s, %s", outpath.toString(), e.displayText()).c_str());
		return -1;
	}
	return 0;
}
#else
int LoadCompiledScript(asIScriptEngine* engine, unsigned char* code, asUINT size) {
	//engine->SetEngineProperty(asEP_INIT_GLOBAL_VARS_AFTER_BUILD, false);
	//engine->SetEngineProperty(asEP_MAX_NESTED_CALLS, 10000);
	asIScriptModule* mod = engine->GetModule("nvgt_game", asGM_ALWAYS_CREATE);
	if (mod == 0)
		return -1;
	mod->SetAccessMask(NVGT_SUBSYSTEM_EVERYTHING);
	NVGTBytecodeStream codestream;
	codestream.set(code, size);
	nvgt_bytecode_istream istr(&codestream);
	BinaryReader br(istr);
	if (!load_serialized_nvgt_plugins(br))
		return -1;
	for (int i = 0; i < asEP_LAST_PROPERTY; i++) {
		UInt64 val;
		br.read7BitEncoded(val);
		engine->SetEngineProperty(asEEngineProp(i), asPWORD(val));
	}
	codestream.reset_cursor(); // Angelscript can produce bytecode load failures as a result of user misconfigurations or bugs, and such failures only include an offset of bytes read maintained by Angelscript internally. The solution in such cases is to breakpoint NVGTBytecodeStream::Read if cursor is greater than the offset given, then one can get more debug info. For that to work, we make sure that the codestream's variable that tracks number of bytes written does not include the count of those written by engine properties, plugins etc. We could theoretically store such data at the end of the stream instead of the beginning and avoid this, but then we are trusting Angelscript to read exactly the number of bytes it's written, and since I don't know how much of a gamble that is, I opted for this instead.
	if (mod->LoadByteCode(&codestream, &g_debug) < 0)
		return -1;
	//engine->SetEngineProperty(asEP_PROPERTY_ACCESSOR_MODE, 2);
	return 0;
}
int LoadCompiledExecutable(asIScriptEngine* engine) {
	FileInputStream fs(Util::Application::instance().commandPath());
	BinaryReader br(fs);
	UInt32 data_location, code_size;
	#ifdef _WIN32
	fs.seekg(60);
	// READ the PE header location
	DWORD header_location;
	br >> header_location;
	fs.seekg(header_location);
	UInt32 sig;
	br >> sig;
	if (sig != IMAGE_NT_SIGNATURE)
		return -1;
	IMAGE_FILE_HEADER ih;
	br.readRaw(reinterpret_cast<char*>(&ih), sizeof(IMAGE_FILE_HEADER));
	// Skip the optional header
	fs.seekg(ih.SizeOfOptionalHeader, ios::cur);
	DWORD offset = 0;
	for (int i = 0; i < ih.NumberOfSections; i++) {
		IMAGE_SECTION_HEADER sh;
		br.readRaw(reinterpret_cast<char*>(&sh), sizeof(IMAGE_SECTION_HEADER));
		if (sh.PointerToRawData + sh.SizeOfRawData > offset)
			offset = sh.PointerToRawData + sh.SizeOfRawData;
	}
	data_location = offset;
	#else
	fs.seekg(-4, std::ios::end);
	br >> data_location;
	#endif
	fs.seekg(data_location);
	if (!load_embedded_packs(br)) return -1;
	br.read7BitEncoded(code_size);
	code_size ^= NVGT_BYTECODE_NUMBER_XOR;
	unsigned char* code = (unsigned char*)malloc(code_size);
	br.readRaw((char*)code, code_size);
	fs.close();
	int r = LoadCompiledScript(engine, code, code_size);
	free(code);
	return r;
}
#endif
int ExecuteScript(asIScriptEngine* engine, const string& scriptFile) {
	asIScriptModule* mod = engine->GetModule("nvgt_game", asGM_ONLY_IF_EXISTS);
	if (!mod) return -1;
	mod->SetAccessMask(NVGT_SUBSYSTEM_EVERYTHING);
	asIScriptFunction* func = mod->GetFunctionByDecl("int main()");
	if (!func)
		func = mod->GetFunctionByDecl("void main()");
	if (!func) {
		g_scriptMessagesInfo = "";
		engine->WriteMessage(scriptFile.c_str(), 0, 0, asMSGTYPE_ERROR, "No entry point found (either 'int main()' or 'void main()'.)");
		return -1;
	}
	asIScriptFunction* prefunc = mod->GetFunctionByDecl("bool preglobals()");
	asIScriptContext* ctx = engine->RequestContext();
	if (!ctx) return -1;
	if (prefunc) {
		if (ctx->Prepare(prefunc) < 0 || ctx->Execute() < 0) {
			engine->ReturnContext(ctx);
			return -1;
		}
		if (!ctx->GetReturnByte()) {
						engine->ReturnContext(ctx);
			return 0;
		}
		engine->ReturnContext(ctx);
	}
	if (mod->ResetGlobalVars(0) < 0) {
		// In this case any extra information is printed as an info message, we usually filter those out.
		g_scriptMessagesErr += g_scriptMessagesInfo;
		return -1;
	}
	ShowAngelscriptMessages(); // Display any warnings or extra info if the user has asked for it.
	g_initialising_globals = false;
	ctx = g_ctxMgr->AddContext(engine, func, true);
	#ifndef NVGT_STUB
	if (g_dbg) {
		cout << "Debugging, waiting for commands. Type 'h' for help." << endl;
		g_dbg->TakeCommands(ctx);
	}
	#endif
	while (g_ctxMgr->ExecuteScripts());
	int r = ctx->GetState();
	if (r != asEXECUTION_FINISHED) {
		if (r == asEXECUTION_EXCEPTION) {
			string exc = GetExceptionInfo(ctx, true);
			string msg = exc + "\r\nCopy to clipboard?";
			int c = question("unhandled exception", msg, false, SDL_MESSAGEBOX_ERROR);
			if (c == 1)
				ClipboardSetText(exc);
			r = -1;
		} else if (r == asEXECUTION_ABORTED)
			r = g_retcode;
		else {
			alert("script terminated", "script terminated unexpectedly");
			r = -1;
		}
		if (r != asEXECUTION_ABORTED)
			g_ctxMgr->DoneWithContext(ctx);
	} else {
		if (func->GetReturnTypeId() == asTYPEID_INT32)
			r = *(int*)ctx->GetAddressOfReturnValue();
		else
			r = 0;
	}
	asIScriptFunction* outfunc = mod->GetFunctionByDecl("void on_exit()");
	if (outfunc) {
		ctx = g_ctxMgr->AddContext(engine, outfunc);
		if (ctx) {
			ctx->Execute();
			g_ctxMgr->DoneWithContext(ctx);
		}
	}
	if (g_ctxMgr) {
		delete g_ctxMgr;
		g_ctxMgr = 0;
	}
	g_ctxPool.clear();
	mod->Discard();
	engine->GarbageCollect();
	return r;
}

#ifndef NVGT_STUB
int PragmaCallback(const string& pragmaText, CScriptBuilder& builder, void* /*userParam*/) {
	asIScriptEngine* engine = builder.GetEngine();
	asUINT pos = 0;
	asUINT length = 0;
	string cleanText;
	while (pos < pragmaText.size()) {
		asETokenClass tokenClass = engine->ParseToken(pragmaText.c_str() + pos, 0, &length);
		if (tokenClass == asTC_IDENTIFIER || tokenClass == asTC_KEYWORD || tokenClass == asTC_VALUE) {
			string token = pragmaText.substr(pos, length);
			cleanText += " " + token;
		}
		if (tokenClass == asTC_UNKNOWN)
			return -1;
		pos += length;
	}
	cleanText.erase(cleanText.begin());
	if (cleanText.substr(0, 8) == "include ") {
		cleanText.erase(0, 8);
		g_IncludeDirs.insert(g_IncludeDirs.begin(), cleanText);
	} else if (cleanText.substr(0, 5) == "stub ")
		g_stub = cleanText.substr(5);
	else if (cleanText.substr(0, 7) == "plugin ") {
		if (!load_nvgt_plugin(cleanText.substr(7)))
			engine->WriteMessage(cleanText.substr(7).c_str(), -1, -1, asMSGTYPE_ERROR, "failed to load plugin");
	} else if (cleanText.substr(0, 18) == "compiled_basename ") {
		g_compiled_basename = cleanText.substr(18);
		if (g_compiled_basename == "*") g_compiled_basename = "";
	} else if (cleanText.substr(0, 9) == "platform ")
		g_platform = cleanText.substr(9);
	else if (cleanText.substr(0, 21) == "bytecode_compression ") {
		g_bcCompressionLevel = strtol(cleanText.substr(21).c_str(), NULL, 10);
		if (g_bcCompressionLevel < 0 || g_bcCompressionLevel > 9) return -1;
	} else if (cleanText == "console") g_make_console = true;
	else return -1;
	return 0;
}
// angelscript debugger stuff taken from asrun sample.
std::string StringToString(void* obj, int /* expandMembers */, CDebugger* /* dbg */) {
	std::string* val = reinterpret_cast<std::string*>(obj);
	std::stringstream s;
	s << "(len=" << val->length() << ") \"";
	if (val->length() < 240)
		s << *val << "\"";
	else
		s << val->substr(0, 240) << "...";
	return s.str();
}
std::string ArrayToString(void* obj, int expandMembers, CDebugger* dbg) {
	CScriptArray* arr = reinterpret_cast<CScriptArray*>(obj);
	std::stringstream s;
	s << "(len=" << arr->GetSize() << ")";
	if (expandMembers > 0) {
		s << " [";
		for (asUINT n = 0; n < arr->GetSize(); n++) {
			s << dbg->ToString(arr->At(n), arr->GetElementTypeId(), expandMembers - 1, arr->GetArrayObjectType()->GetEngine());
			if (n < arr->GetSize() - 1)
				s << ", ";
		}
		s << "]";
	}
	return s.str();
}
std::string DictionaryToString(void* obj, int expandMembers, CDebugger* dbg) {
	CScriptDictionary* dic = reinterpret_cast<CScriptDictionary*>(obj);
	std::stringstream s;
	s << "(len=" << dic->GetSize() << ")";
	if (expandMembers > 0) {
		s << " [";
		asUINT n = 0;
		for (CScriptDictionary::CIterator it = dic->begin(); it != dic->end(); it++, n++) {
			s << "[" << it.GetKey() << "] = ";
			const void* val = it.GetAddressOfValue();
			int typeId = it.GetTypeId();
			asIScriptContext* ctx = asGetActiveContext();
			s << dbg->ToString(const_cast<void*>(val), typeId, expandMembers - 1, ctx ? ctx->GetEngine() : 0);
			if (n < dic->GetSize() - 1)
				s << ", ";
		}
		s << "]";
	}
	return s.str();
}
std::string DateTimeToString(void *obj, int expandMembers, CDebugger *dbg) {
	Poco::DateTime*dt = reinterpret_cast<Poco::DateTime*>(obj);
	std::stringstream s;
	s << "{" << dt->year() << "-" << dt->month() << "-" << dt->day() << " ";
	s << dt->hour() << ":" << dt->minute() << ":" << dt->second() << "}";
	return s.str(); 
}
std::string Vector3ToString(void* obj, int expandMembers, CDebugger* dbg) {
	Vector3* v = reinterpret_cast<Vector3*>(obj);
	std::stringstream s;
	s << "{" << v->x << ", " << v->y << ", " << v->z << "}";
	return s.str();
}
#ifdef _WIN32
BOOL WINAPI debugger_ctrlc(DWORD event) {
	if (event != CTRL_C_EVENT || !g_dbg || g_dbg->IsTakingCommands()) return FALSE;
	g_ASDebugBreak = true;
	return TRUE;
}
#endif
void InitializeDebugger(asIScriptEngine* engine) {
	#ifdef _WIN32
	SetConsoleCtrlHandler(debugger_ctrlc, TRUE);;
	#endif
	g_dbg = new CDebugger();
	g_dbg->SetEngine(engine);
	g_dbg->RegisterToStringCallback(engine->GetTypeInfoByName("string"), StringToString);
	g_dbg->RegisterToStringCallback(engine->GetTypeInfoByName("array"), ArrayToString);
	g_dbg->RegisterToStringCallback(engine->GetTypeInfoByName("dictionary"), DictionaryToString);
	g_dbg->RegisterToStringCallback(engine->GetTypeInfoByName("datetime"), DateTimeToString);
	g_dbg->RegisterToStringCallback(engine->GetTypeInfoByName("vector"), Vector3ToString);
}
void asDebugBreak() {
	if (!g_dbg) return;
	cout << "script debug break" << endl;
	g_dbg->TakeCommands(asGetActiveContext());
}
void asDebuggerAddFileBreakpoint(const std::string& file, int line) { if (g_dbg) g_dbg->AddFileBreakPoint(file, line); }
void asDebuggerAddFuncBreakpoint(const std::string& func) { if (g_dbg) g_dbg->AddFuncBreakPoint(func); }
#else
void asDebugBreak() {} // Debugger not present for compiled executables.
void asDebuggerAddFileBreakpoint(const std::string& file, int line) {}
void asDebuggerAddFuncBreakpoint(const std::string& func) {}
#endif

asIScriptContext* RequestContextCallback(asIScriptEngine* engine, void* /*param*/) {
	asIScriptContext* ctx = 0;
	int pool_size = 0;
	{
		ScopedLock<Mutex> l(g_ctxPoolMutex);
		pool_size = g_ctxPool.size();
		if (pool_size) {
			ctx = g_ctxPool.back();
			g_ctxPool.pop_back();
		}
	}
	if (!pool_size) {
		ctx = engine->CreateContext();
		ctx->SetExceptionCallback(asFUNCTION(ExceptionHandlerCallback), NULL, asCALL_CDECL);
		ctx->SetLineCallback(asFUNCTION(nvgt_line_callback), NULL, asCALL_CDECL);
	}
	return ctx;
}
void ReturnContextCallback(asIScriptEngine* engine, asIScriptContext* ctx, void* /*param*/) {
	ctx->Unprepare();
	ScopedLock<Mutex> l(g_ctxPoolMutex);
	g_ctxPool.push_back(ctx);
}
void ExceptionHandlerCallback(asIScriptContext* ctx, void* obj) {
	g_last_exception_callstack = get_call_stack();
}

// Try not to register things here unless absolutely no other place can be found for them.
void RegisterUnsorted(asIScriptEngine* engine) {
	engine->SetDefaultAccessMask(NVGT_SUBSYSTEM_GENERAL);
	engine->RegisterGlobalProperty("const string NVGT_VERSION", (void*)&NVGT_VERSION);
	engine->RegisterGlobalProperty("const string NVGT_VERSION_COMMIT_HASH", (void*)&NVGT_VERSION_COMMIT_HASH);
	engine->RegisterGlobalProperty("const string NVGT_VERSION_BUILD_TIME", (void*)&NVGT_VERSION_BUILD_TIME);
	engine->RegisterGlobalProperty("const uint NVGT_VERSION_BUILD_TIMESTAMP", (void*)&NVGT_VERSION_BUILD_TIMESTAMP);
	engine->RegisterGlobalProperty("const int NVGT_VERSION_MAJOR", (void*)&NVGT_VERSION_MAJOR);
	engine->RegisterGlobalProperty("const int NVGT_VERSION_MINOR", (void*)&NVGT_VERSION_MINOR);
	engine->RegisterGlobalProperty("const int NVGT_VERSION_PATCH", (void*)&NVGT_VERSION_PATCH);
	engine->RegisterGlobalProperty("const string NVGT_VERSION_TYPE", (void*)&NVGT_VERSION_TYPE);
	engine->RegisterGlobalFunction("void debug_break()", asFUNCTION(asDebugBreak), asCALL_CDECL);
	engine->RegisterGlobalFunction("void debug_add_file_breakpoint(const string&in, int)", asFUNCTION(asDebuggerAddFileBreakpoint), asCALL_CDECL);
	engine->RegisterGlobalFunction("void debug_add_func_breakpoint(const string&in)", asFUNCTION(asDebuggerAddFuncBreakpoint), asCALL_CDECL);
	engine->RegisterGlobalProperty("const string[]@ ARGS", &g_command_line_args);
}
