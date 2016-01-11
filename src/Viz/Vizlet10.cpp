/*
	Copyright (c) 2011-2013 Tim Thompson <me@timthompson.com>

	Permission is hereby granted, free of charge, to any person obtaining
	a copy of this software and associated documentation files
	(the "Software"), to deal in the Software without restriction,
	including without limitation the rights to use, copy, modify, merge,
	publish, distribute, sublicense, and/or sell copies of the Software,
	and to permit persons to whom the Software is furnished to do so,
	subject to the following conditions:

	The above copyright notice and this permission notice shall be
	included in all copies or substantial portions of the Software.

	Any person wishing to distribute modifications to the Software is
	requested to send the modifications to the original developer so that
	they can be incorporated into the canonical version.

	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
	EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
	MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
	IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
	ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
	CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
	WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include <pthread.h>

#include <iostream>
#include <sstream>
#include <fstream>
#include <strstream>
#include <cstdlib> // for srand, rand
#include <ctime>   // for time
#include <sys/stat.h>

#include "UT_SharedMem.h"
#include "NosuchUtil.h"
#include "NosuchMidi.h"
#include "NosuchScheduler.h"
#include "Vizlet10.h"
#include "Vizlet10util.h"
#include "NosuchJSON.h"

#include "osc/OscOutboundPacketStream.h"
#include "osc/OscReceivedElements.h"
#include "NosuchOsc.h"

////////////////////////////////////////////////////////////////////////////////////////////////////
//  Plugin information
////////////////////////////////////////////////////////////////////////////////////////////////////
#ifdef _WIN32

// These functions need to be defined in a vizlet's source file.
extern std::string vizlet10_name();
extern bool vizlet10_setdll(std::string(dllpath));
CFF10PluginInfo& vizlet10_plugininfo();

void vizlet10_setid(CFF10PluginInfo& plugininfo, std::string name)
{
	char id[5];
	// Compute a hash of the plugin name and use two 4-bit values
	// from it to produce the last 2 characters of the unique ID.
	// It's possible there will be a collision.
	int hash = 0;
	for ( const char* p = name.c_str(); *p!='\0'; p++ ) {
		hash += *p;
	}
	id[0] = 'V';
	id[1] = 'Z';
	id[2] = 'A' + (hash & 0xf);
	id[3] = 'A' + ((hash >> 4) & 0xf);
	id[4] = '\0';
	plugininfo.SetPluginIdAndName(id,name.c_str());
}

char dllpath[MAX_PATH];
std::string dllpathstr;

std::string dll_pathname() {
	return(dllpathstr);
}

BOOL APIENTRY DllMain(HANDLE hModule, DWORD  ul_reason_for_call, LPVOID lpReserved)
{
	GetModuleFileNameA((HMODULE)hModule, dllpath, MAX_PATH);
	dllpathstr = std::string(dllpath);

	if (ul_reason_for_call == DLL_PROCESS_ATTACH ) {
		// Initialize once for each new process.
		// Return FALSE if we fail to load DLL.
		dllpathstr = NosuchToLower(dllpathstr);
		if ( ! vizlet10_setdll(dllpathstr) ) {
			DEBUGPRINT(("vizlet10_setdll failed"));
			return FALSE;
		}
		vizlet10_setdll(dllpathstr);
		std::string s = vizlet10_name();
		DEBUGPRINT1(("After vizlet10_setdll, dllpathstr=%s vizlet10_name=%s",dllpathstr.c_str(),s.c_str()));
		vizlet10_setid(vizlet10_plugininfo(),s);
		DEBUGPRINT1(("DLL_PROCESS_ATTACH dll=%s",dllpath));
	}
	if (ul_reason_for_call == DLL_PROCESS_DETACH ) {
		DEBUGPRINT1(("DLL_PROCESS_DETACH dll=%s",dllpath));
	}
	if (ul_reason_for_call == DLL_THREAD_ATTACH ) {
		DEBUGPRINT1(("DLL_THREAD_ATTACH dll=%s",dllpath));
	}
	if (ul_reason_for_call == DLL_THREAD_DETACH ) {
		DEBUGPRINT1(("DLL_THREAD_DETACH dll=%s",dllpath));
	}
    return TRUE;
}

#endif

Vizlet10::Vizlet10() {

	DEBUGPRINT1(("--- Vizlet10 constructor, dll_pathname=%s",dll_pathname().c_str()));

	VizParams::Initialize();
	m_defaultparams = new SpriteVizParams();
	m_useparamcache = false;
	m_callbacksInitialized = false;
	m_passthru = true;
	m_defaultmidiparams = defaultParams();
	m_framenum = 0;
	m_image = NULL;
	m_imagesize = cvSize(0,0);
	m_VideoInfo.BitDepth = FF_DEPTH_24;
	m_VideoInfo.FrameHeight = 0;
	m_VideoInfo.FrameWidth = 0;
	m_VideoInfo.Orientation = FF_ORIENTATION_TL;
	m_vizserver = VizServer::GetServer();
	m_viztag = vizlet10_name();
	m_mf = MidiFilter();  // ALL Midi
	m_cf = CursorFilter();  // ALL Cursors

#ifdef PALETTE_PYTHON
	_recompileFunc = NULL;
	_python_enabled = FALSE;
	m_python_events_disabled = TRUE;
	NosuchLockInit(&python_mutex,"python");
#endif

	NosuchLockInit(&vizlet10_mutex,"vizlet");

	NosuchLockInit(&json_mutex,"json");
	json_cond = PTHREAD_COND_INITIALIZER;
	json_pending = false;

	m_disabled = false;
	m_disable_on_exception = false;

	m_stopped = false;

	// The most common reason for being disabled at this point
	// is when the config JSON file can't be parsed.
	if ( m_disabled ) {
		DEBUGPRINT(("WARNING! Vizlet10 (viztag=%s) has been disabled!",VizTag().c_str()));
	}

	SetTimeSupported(true);

	SetMinInputs(1);
	SetMaxInputs(1);

#ifdef VIZTAG_PARAMETER
	SetParamInfo(0,"viztag", FF_TYPE_TEXT, VizTag().c_str());
#endif
}

Vizlet10::~Vizlet10()
{
	DEBUGPRINT(("--- Vizlet10 destructor is called viztag=%s", m_viztag.c_str()));
	_stopstuff();
}

DWORD Vizlet10::SetTime(double tm) {
	m_vizserver->SetTime(tm);
	return FF_SUCCESS;
}

DWORD Vizlet10::SetParameter(const SetParameterStruct* pParam) {

	return FF_FAIL;
#ifdef VIZTAG_PARAMETER
	DWORD r = FF_FAIL;

	// make sure the VizServer is started.
	StartVizServer();
	InitCallbacks();

	switch ( pParam->ParameterNumber ) {
	case 0:		// shape
		if ( VizTag() != std::string(pParam->u.NewTextValue) ) {
			SetVizTag(pParam->u.NewTextValue);
			m_af = ApiFilter(VizTag().c_str());
			ChangeVizTag(pParam->u.NewTextValue);
		}
		r = FF_SUCCESS;
	}
	return r;
#endif
}

DWORD Vizlet10::GetParameter(DWORD n) {
#ifdef VIZTAG_PARAMETER
	switch ( n ) {
	case 0:		// shape
		return (DWORD)(VizTag().c_str());
	}
#endif
	return FF_FAIL;
}

char* Vizlet10::GetParameterDisplay(DWORD n)
{
#ifdef VIZTAG_PARAMETER
	switch ( n ) {
	case 0:
	    strncpy_s(_disp,DISPLEN,VizTag().c_str(),VizTag().size());
	    _disp[DISPLEN-1] = 0;
		return _disp;
	}
#endif
	return "";
}

const char* vizlet10_json(void* data,const char *method, cJSON* params, const char* id) {
	Vizlet10* v = (Vizlet10*)data;
	if ( v == NULL ) {
		static std::string err = error_json(-32000,"v is NULL is vizlet10_json?",id);
		return err.c_str();
	}
	else {
#if 0
		// A few methods are built-in.  If it isn't one of those,
		// it is given to the plugin to handle.
		if (std::string(method) == "description") {
			std::string desc = vizlet10_plugininfo().GetPluginExtendedInfo()->Description;
			v->m_json_result = jsonStringResult(desc, id);
		} else if (std::string(method) == "about") {
			std::string desc = vizlet10_plugininfo().GetPluginExtendedInfo()->About;
			v->m_json_result = jsonStringResult(desc, id);
		} else {
			v->m_json_result = v->processJsonAndCatchExceptions(std::string(method), params, id);
		}
#endif
		v->m_json_result = v->processJsonAndCatchExceptions(std::string(method), params, id);
		return v->m_json_result.c_str();
	}
}

void vizlet10_osc(void* data,const char *source, const osc::ReceivedMessage& m) {
	Vizlet10* v = (Vizlet10*)data;
	NosuchAssert(v);
	v->processOsc(source,m);
}

void vizlet10_midiinput(void* data,MidiMsg* m) {
	Vizlet10* v = (Vizlet10*)data;
	NosuchAssert(v);
	v->processMidiInput(m);
}

void vizlet10_midioutput(void* data,MidiMsg* m) {
	Vizlet10* v = (Vizlet10*)data;
	NosuchAssert(v);
	v->processMidiOutput(m);
}

void vizlet10_cursor(void* data,VizCursor* c, int downdragup) {
	Vizlet10* v = (Vizlet10*)data;
	NosuchAssert(v);
	v->processCursor(c,downdragup);
}

void vizlet10_keystroke(void* data,int key, int downup) {
	Vizlet10* v = (Vizlet10*)data;
	NosuchAssert(v);
	v->processKeystroke(key,downup);
}

void Vizlet10::advanceCursorTo(VizCursor* c, double tm) {
	m_vizserver->AdvanceCursorTo(c,tm);
}

#ifdef VIZTAG_PARAMETER
void Vizlet10::ChangeVizTag(const char* p) {
	m_vizserver->ChangeVizTag(Handle(),p);
}
#endif

void Vizlet10::_startApiCallbacks(const char* apiprefix, void* data) {
	NosuchAssert(m_vizserver);
	m_vizserver->AddJsonCallback(Handle(),apiprefix,vizlet10_json,data);
}

void Vizlet10::_startMidiCallbacks(MidiFilter mf, void* data) {
	NosuchAssert(m_vizserver);
	m_vizserver->AddMidiInputCallback(Handle(),mf,vizlet10_midiinput,data);
	m_vizserver->AddMidiOutputCallback(Handle(),mf,vizlet10_midioutput,data);
}

void Vizlet10::_startCursorCallbacks(CursorFilter cf, void* data) {
	NosuchAssert(m_vizserver);
	m_vizserver->AddCursorCallback(Handle(),cf,vizlet10_cursor,data);
}

void Vizlet10::_startKeystrokeCallbacks(void* data) {
	NosuchAssert(m_vizserver);
	m_vizserver->AddKeystrokeCallback(Handle(),vizlet10_keystroke,data);
}

void Vizlet10::_stopCallbacks() {
	_stopApiCallbacks();
	_stopMidiCallbacks();
	_stopCursorCallbacks();
	_stopKeystrokeCallbacks();
}

void Vizlet10::_stopApiCallbacks() {
	NosuchAssert(m_vizserver);
	m_vizserver->RemoveJsonCallback(Handle());
}

void Vizlet10::_stopMidiCallbacks() {
	NosuchAssert(m_vizserver);
	m_vizserver->RemoveMidiInputCallback(Handle());
	m_vizserver->RemoveMidiOutputCallback(Handle());
}

void Vizlet10::_stopCursorCallbacks() {
	NosuchAssert(m_vizserver);
	m_vizserver->RemoveCursorCallback(Handle());
}

void Vizlet10::_stopKeystrokeCallbacks() {
	NosuchAssert(m_vizserver);
	m_vizserver->RemoveKeystrokeCallback(Handle());
}

double Vizlet10::GetTime() {
	return m_vizserver->GetTime();
}

int Vizlet10::SchedulerCurrentClick() {
	if ( m_vizserver == NULL ) {
		DEBUGPRINT(("Vizlet10::CurrentClick() - _vizserver is NULL!"));
		return 0;
	} else {
		return m_vizserver->SchedulerCurrentClick();
	}
}

#if 0
void Vizlet10::SendMidiMsg() {
	DEBUGPRINT(("Vizlet10::SendMidiMsg called - this should go away eventually"));
	MidiMsg* msg = MidiNoteOn::make(1,80,100);
	// _vizserver->MakeNoteOn(1,80,100);
	m_vizserver->SendMidiMsg(msg);
}
#endif

void Vizlet10::_stopstuff() {
	if ( m_stopped )
		return;
	m_stopped = true;
	_stopCallbacks();
	if ( m_vizserver ) {
		int ncb = m_vizserver->NumCallbacks();
		int mcb = m_vizserver->MaxCallbacks();
		DEBUGPRINT1(("Vizlet10::_stopstuff - VizServer has %d callbacks, max=%d!",ncb,mcb));
		if ( ncb == 0 && mcb > 0 ) {
			m_vizserver->Stop();
		}
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////
//  Methods
////////////////////////////////////////////////////////////////////////////////////////////////////

void
Vizlet10::QueueMidiPhrase(MidiPhrase* ph, click_t clk) {
	m_vizserver->QueueMidiPhrase(ph,clk);
}

void
Vizlet10::QueueMidiMsg(MidiMsg* m, click_t clk) {
	m_vizserver->QueueMidiMsg(m,clk);
}

void
Vizlet10::QueueClear() {
	m_vizserver->QueueClear();
}

void
Vizlet10::StartVizServer() {
	if ( ! m_vizserver->Started() ) {
		m_vizserver->Start();
		srand( (unsigned int)(m_vizserver->GetTime()) );
	}
}

void
Vizlet10::InitCallbacks() {
	if ( ! m_callbacksInitialized ) {

		_startApiCallbacks(m_viztag.c_str(),(void*)this);
		_startMidiCallbacks(m_mf,(void*)this);
		_startCursorCallbacks(m_cf,(void*)this);
		_startKeystrokeCallbacks((void*)this);

		m_callbacksInitialized = true;
	}
}

DWORD Vizlet10::SetVideoInfo(const VideoInfoStruct* pvi)
{
	if (pvi) {
		memcpy(&m_VideoInfo, pvi, sizeof(VideoInfoStruct));

		int bytes;
		if (pvi->BitDepth == 1) {
			bytes = 3;
		}
		else if (pvi->BitDepth == 2) {
			bytes = 4;
		}
		else {
			// this shouldn't happen if the host is checking the capabilities properly
			return FF_FAIL;
		}

		m_imagesize = cvSize(pvi->FrameWidth, pvi->FrameHeight);
		m_image = cvCreateImageHeader(m_imagesize, IPL_DEPTH_8U, bytes);
		return FF_SUCCESS;
	}
	return FF_FAIL;
}

IplImage* Vizlet10::FrameImage() {
	return m_image;
}

DWORD Vizlet10::ProcessFrame(void* pFrame)
{
	if (m_stopped) {
		return FF_SUCCESS;
	}
	if (m_disabled) {
		return FF_SUCCESS;
	}
	StartVizServer();
	InitCallbacks();

	m_framenum++;

	NosuchLock(&json_mutex, "json");
	if (json_pending) {
		// Execute json stuff and generate response

		m_json_result = processJsonAndCatchExceptions(json_method, json_params, json_id);
		json_pending = false;
		int e = pthread_cond_signal(&json_cond);
		if (e) {
			DEBUGPRINT(("ERROR from pthread_cond_signal e=%d\n", e));
		}
	}
	NosuchUnlock(&json_mutex, "json");

	LockVizlet();

	bool gotexception = false;
	try {
		CATCH_NULL_POINTERS;

		VideoInfoStruct* vid = GetVideoInfo();
		bool r;
		switch (vid->BitDepth) {
		case 1:
			m_image->origin = 1;
			m_image->imageData = (char*)pFrame;
			r = processFrame24Bit();
			break;
		case 2:
			m_image->origin = 1;
			m_image->imageData = (char*)pFrame;
			r = processFrame32Bit();
			break;
		default:
			r = false;
			break;
		}
		if (!r) {
			DEBUGPRINT(("Vizlet10::ProcessFrame* returned failure? (r=%d)\n", r));
			gotexception = true;
		}
	}
	catch (NosuchException& e) {
		DEBUGPRINT(("NosuchException in Vizlet10::ProcessFrame : %s", e.message()));
		gotexception = true;
	}
	catch (...) {
		DEBUGPRINT(("UNKNOWN Exception in Vizlet10::ProcessFrame!"));
		gotexception = true;
	}

	if (gotexception && m_disable_on_exception) {
		DEBUGPRINT(("DISABLING Vizlet10 due to exception!!!!!"));
		m_disabled = true;
	}

	UnlockVizlet();

	return FF_SUCCESS;
}

DWORD Vizlet10::ProcessFrameCopy(ProcessFrameCopyStruct* pFrameData) {
	return FF_FAIL;
}

void Vizlet10::LockVizlet() {
	NosuchLock(&vizlet10_mutex,"Vizlet10");
}

void Vizlet10::UnlockVizlet() {
	NosuchUnlock(&vizlet10_mutex,"Vizlet10");
}

std::string Vizlet10::processJsonAndCatchExceptions(std::string meth, cJSON *params, const char *id) {
	std::string r;
	try {
		CATCH_NULL_POINTERS;

		// Here, we should intercept and interpret some APIs that are common to all vizlets
		if ( meth == "echo"  ) {
			std::string val = jsonNeedString(params,"value");
			r = jsonStringResult(val,id);
		} else if ( meth == "time"  ) {
			r = jsonDoubleResult(GetTime(),id);
		} else if ( meth == "name"  ) {
		    std::string nm = CopyFFString16((const char *)(vizlet10_plugininfo().GetPluginInfo()->PluginName));
			r = jsonStringResult(nm,id);
		} else if ( meth == "dllpathname"  ) {
			r = jsonStringResult(dll_pathname(),id);
		} else {
			// If not one of the standard vizlet APIs, call the vizlet-plugin-specific API processor
			r = processJson(meth,params,id);
		}
	} catch (NosuchException& e) {
		std::string s = NosuchSnprintf("NosuchException in executeJson!! - %s",e.message());
		r = error_json(-32000,s.c_str(),id);
	} catch (...) {
		// This doesn't seem to work - it doesn't seem to catch other exceptions...
		std::string s = NosuchSnprintf("Some other kind of exception occured in executeJson!?");
		r = error_json(-32000,s.c_str(),id);
	}
	return r;
}

SpriteVizParams*
Vizlet10::findSpriteVizParams(std::string cachename) {
	std::map<std::string,SpriteVizParams*>::iterator it = m_paramcache.find(cachename);
	if ( it == m_paramcache.end() ) {
		return NULL;
	}
	return it->second;
}

SpriteVizParams*
readSpriteVizParams(std::string path) {
	std::string err;
	cJSON* json = jsonReadFile(path,err);
	if ( !json ) {
		DEBUGPRINT(("Unable to load vizlet params: path=%s, err=%s",
			path.c_str(),err.c_str()));
		return NULL;
	}
	SpriteVizParams* s = new SpriteVizParams();
	s->loadJson(json);
	// XXX - should json be freed, here?
	return s;
}

std::string
Vizlet10::VizPath2ConfigName(std::string path) {
	size_t pos = path.find_last_of("/\\");
	if (pos != path.npos) {
		path = path.substr(pos+1);
	}
	pos = path.find_last_of(".");
	if (pos != path.npos) {
		path = path.substr(0,pos);
	}
	return(path);
}

SpriteVizParams*
Vizlet10::getSpriteVizParams(std::string path) {
	if (m_useparamcache) {
		std::map<std::string, SpriteVizParams*>::iterator it = m_paramcache.find(path);
		if (it == m_paramcache.end()) {
			m_paramcache[path] = readSpriteVizParams(path);
			return m_paramcache[path];
		}
		else {
			return it->second;
		}
	}
	else {
		return readSpriteVizParams(path);
	}
}

SpriteVizParams*
Vizlet10::checkSpriteVizParamsAndLoadIfModifiedSince(std::string fname, std::time_t& lastcheck, std::time_t& lastupdate) {
	std::string path = SpriteVizParamsPath(fname);
	std::time_t throttle = 1;  // don't check more often than this number of seconds
	std::time_t tm = time(0);
	if ((tm - lastcheck) < throttle) {
		return NULL;
	}
	lastcheck = tm;
	struct _stat statbuff;
	int e = _stat(path.c_str(), &statbuff);
	if (e != 0) {
		throw NosuchException("Error in checkAndLoad - e=%d", e);
	}
	if (lastupdate == statbuff.st_mtime) {
		return NULL;
	}
	SpriteVizParams* p = getSpriteVizParams(path);
	if (!p) {
		throw NosuchException("Bad params file? path=%s", path.c_str());
	}
	lastupdate = statbuff.st_mtime;
	return p;
}