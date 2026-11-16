#include "loader/runtimeChildProcess.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace Loader::RuntimeChild;
namespace {
void Check(bool ok,const char* why) { if(!ok) { throw std::runtime_error(why); } }
template<typename F> void Until(F predicate) { const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(8); while(!predicate()) {
	MSG message; while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
	Check(std::chrono::steady_clock::now()<end,"bounded embedded-window wait"); Sleep(2);
} }
HWND Parent() {
	WNDCLASSW cls {}; cls.lpfnWndProc=DefWindowProcW; cls.hInstance=GetModuleHandleW(nullptr); cls.lpszClassName=L"KytyHiddenVulkanParentProbe"; RegisterClassW(&cls);
	RECT rect {0,0,1280,720}; AdjustWindowRectEx(&rect,WS_OVERLAPPEDWINDOW, FALSE,0);
	return CreateWindowExW(0,cls.lpszClassName,L"Hidden test parent",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,-32000,-32000,rect.right-rect.left,rect.bottom-rect.top,nullptr,nullptr,cls.hInstance,nullptr);
}
}
int main(int argc,char** argv) try {
	Check(argc==2,"window helper path"); SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	const auto root=std::filesystem::current_path()/("hidden-window-fixture-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64())); std::filesystem::create_directory(root);
	for(unsigned iteration=0;iteration!=3;++iteration) {
		HWND parent=Parent(); Check(parent,"create hidden parent"); const auto original_style=GetWindowLongPtrW(parent,GWL_STYLE);
		Spawn spawn; spawn.executable=std::filesystem::absolute(argv[1]); spawn.directory=root; spawn.log=root/("window-"+std::to_string(iteration)+".log");
		spawn.key={105,0,iteration+1,0x123456789,GetTickCount64()}; spawn.parent_window=reinterpret_cast<uint64_t>(parent);
		spawn.arguments={L"--parent",std::to_wstring(spawn.parent_window),L"--parent-pid",std::to_wstring(GetCurrentProcessId())};
		if(iteration==2) { spawn.arguments.insert(spawn.arguments.end(),{L"--fail-focus",L"1"}); }
		uint32_t error=0; auto p=Process::Create(spawn,&error); Check(bool(p)&&!error,"create hidden Vulkan subprocess"); HWND child=nullptr;
		Until([&] { auto tick=p->Poll(); for(size_t i=0;i<tick.count;++i) { if(tick.messages[i].kind==Kind::Window) { child=reinterpret_cast<HWND>(tick.messages[i].value[0]); } }
			if(p->Retired()) { throw std::runtime_error("Vulkan helper early exit="+std::to_string(p->ExitCode())+" error="+std::to_string(p->Error())); }
			return p->PresentationReady(); });
		Check(child&&GetParent(child)==parent&&(GetWindowLongPtrW(child,GWL_STYLE)&WS_CHILD),"real child-owned embedded HWND");
		RECT initial {}; Check(GetClientRect(parent,&initial)&&initial.right==1280&&initial.bottom==720,"1280x720 host client independent of guest extent");
		Check(!IsWindowVisible(parent)&&GetForegroundWindow()!=parent&&GetForegroundWindow()!=child,"hidden test never steals foreground");
		Check(p->Focus(true,true,1),"request embedded show"); Until([&] { p->Poll(); return p->FocusAcknowledged()==1; });
		if(iteration==2) {
			Check(p->FocusResult()<0 && !(GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE),"partial input/audio focus failure cannot leave child visible");
			p->Close(false); Until([&] { return p->Poll().retired; }); Check(!IsWindow(child)&&p->ExitCode()==0,"failed focus child actually retires");
			DestroyWindow(parent); continue;
		}
		RECT rect {0,0,1024,576}; AdjustWindowRectEx(&rect,WS_OVERLAPPEDWINDOW,FALSE,0);
		Check(SetWindowPos(parent,nullptr,0,0,rect.right-rect.left,rect.bottom-rect.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE),"resize only own hidden parent");
		Until([&] { p->Poll(); RECT actual{}; GetClientRect(child,&actual); return actual.right==1024&&actual.bottom==576; });
		Check(p->Focus(false,false,2),"Home hides rather than exits"); Until([&] { p->Poll(); return p->FocusAcknowledged()==2; });
		Check(!(GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE)&&!p->Retired(),"full Home keeps child process alive");
		Check(p->Focus(true,true,3),"resume same process"); Until([&] { p->Poll(); return p->FocusAcknowledged()==3; });
		Check(GetWindowLongPtrW(parent,GWL_STYLE)==original_style&&!IsZoomed(parent),"user top-level state not forced/maximized");
		if(iteration==0) { p->Close(false); } else { DestroyWindow(parent); parent=nullptr; }
		Until([&] { return p->Poll().retired; }); Check(p->ExitCode()==0,"actual Vulkan child shutdown/drain");
		Check(!IsWindow(child),"child HWND retired only after process wait"); if(parent) { DestroyWindow(parent); }
		std::ifstream log(spawn.log); std::string contents((std::istreambuf_iterator<char>(log)),{});
		Check(contents.find("accepted hidden Vulkan present 2 1024x576")!=std::string::npos,"real Vulkan swapchain recreation/present after parent resize");
	}
	std::cout<<"hidden embedded HWND/Vulkan/Home/resume tests passed\n"; return 0;
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
