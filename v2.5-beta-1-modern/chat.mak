# chat.mak - modern nmake makefile for Microsoft Chat (v2.5-beta-1-modern)
# Builds CChat.exe with Visual Studio 2026 C++/MFC (static), with every C++
# translation unit in MSVC's post-C++23 working-draft mode (/std:c++latest),
# replacing the original NT DDK BUILD.EXE (sources/dirs) system.
#
#   call "<VS2026>\VC\Auxiliary\Build\vcvars32.bat"
#   nmake /f chat.mak CFG="chat - Win32 Debug"      (asserts + TRACE for DebugView)
#   nmake /f chat.mak CFG="chat - Win32 Release"    (optimized, no debug asserts)
#
# Notes:
#  - Precompiled headers are intentionally disabled for robustness.
#  - Delay-load helper (dlylddll.c) and NetMeeting (nmproto) are excluded.
#  - icchat_i.c / icchat.h are generated from base\icchat.idl via MIDL.
#  - Debug -> .\Debug\CChat.exe ; Release -> .\Release\CChat.exe.

!IF "$(CFG)" == ""
CFG=chat - Win32 Debug
!ENDIF

!IF "$(CFG)" == "chat - Win32 Release"
OUTDIR=.\Release
INTDIR=.\Release
CPP_CFG=/MT /O2 /GL /Gy /Gw /D "NDEBUG"
RSC_CFG=/d "NDEBUG"
!ELSE
OUTDIR=.\Debug
INTDIR=.\Debug
CPP_CFG=/MTd /Od /D "_DEBUG"
RSC_CFG=/d "_DEBUG"
!ENDIF

CPP=cl.exe
RSC=rc.exe
MIDL=midl.exe
LINK32=link.exe

ARTINC=..\artifacts\inc
ARTLIB=..\artifacts\lib\i386
PORTABLELIB=..\artifacts\lib

# Modern native resources are generated before NMAKE. Keep the generated make
# fragment mandatory: omitting it would produce a valid-looking executable that
# silently uses only the legacy bitmap/DIB fallback artwork.
MODERN_ICON_ROOT=..\portable\assets\icons\generated
MODERN_ICON_RCINC=$(MODERN_ICON_ROOT)\windows\modern-icon-assets.rcinc
MODERN_ICON_MAKINC=$(MODERN_ICON_ROOT)\windows\modern-icon-assets.makinc

!IF !EXIST("$(MODERN_ICON_MAKINC)")
!ERROR Missing generated modern icon dependency include. Run: python ..\scripts\build-modern-icons.py generate
!ENDIF
!IF !EXIST("$(MODERN_ICON_RCINC)")
!ERROR Missing generated modern icon resource include. Run: python ..\scripts\build-modern-icons.py generate
!ENDIF
!INCLUDE "$(MODERN_ICON_MAKINC)"

# /Zi is kept in both configs so Release is still debuggable (it emits a PDB but
# does not disable optimization). The MFC/CRT static libraries are selected
# automatically by the _DEBUG/NDEBUG define + /MT[d]. cpp26mode.h is force-
# included so every C++ source proves that these language settings are active.
CPP26_FLAGS=/std:c++latest /permissive- /EHsc /Zc:__cplusplus /Zc:preprocessor \
 /Zc:forScope /Zc:strictStrings /Zc:wchar_t /Zc:inline /Zc:externConstexpr \
 /Zc:lambda /Zc:twoPhase /Zc:throwingNew /Zc:ternary /volatile:iso

CPP_PROJ=/nologo $(CPP_CFG) /W4 /Zi $(CPP26_FLAGS) /FI"cpp26mode.h" \
 /D "WIN32" /D "_WINDOWS" /D "_MBCS" \
 /D "MBEDTLS_USER_CONFIG_FILE=\"comicchat/mbedtls_user_config.h\"" \
 /I "." /I "$(ARTINC)" \
 /I "..\portable\include" /I "..\third_party\libuv\include" \
 /I "..\third_party\mbedtls\include" \
 /Fo"$(INTDIR)\\" /Fd"$(INTDIR)\\" /c

# Keep the generated MIDL glue and legacy delay-loader in C mode. The product's
# hand-written C++ surface is C++26-mode-only; generated/third-party C is not
# misrepresented as C++ by forcing /TP.
C_PROJ=/nologo $(CPP_CFG) /W3 /Zi /D "WIN32" /D "_WINDOWS" /D "_MBCS" \
 /D "MBEDTLS_USER_CONFIG_FILE=\"comicchat/mbedtls_user_config.h\"" \
 /I "." /I "$(ARTINC)" /I "..\portable\include" \
 /I "..\third_party\mbedtls\include" \
 /Fo"$(INTDIR)\\" /Fd"$(INTDIR)\\" /c

RSC_PROJ=/l 0x409 /fo"$(INTDIR)\chat.res" /i "." /i "$(ARTINC)" $(RSC_CFG)

RESOURCE_BITMAP_INPUTS= \
	res\balloons.bmp \
	res\tiki2.bmp \
	res\toolbar.bmp \
	res\tabbar.bmp \
	res\member.bmp \
	res\oldnew.bmp \
	res\connect.bmp \
	res\stopped.bmp \
	res\inactive.bmp \
	res\active.bmp \
	res\texttool.bmp \
	res\usertool.bmp \
	res\fc_hap_l.bmp \
	res\fc_laf_l.bmp \
	res\fc_sho_l.bmp \
	res\fc_ang_l.bmp \
	res\fc_sad_l.bmp \
	res\fc_sca_l.bmp \
	res\fc_bor_l.bmp \
	res\fc_coy_l.bmp \
	res\palette.dib

RESOURCE_INPUTS= \
	chat.rc \
	resource.h \
	cchat.rcv \
	chatver.rc \
	chatver.h \
	res\chat.rc2 \
	$(ARTINC)\textview.rc \
	$(ARTINC)\tvres.h \
	$(ARTINC)\hand.cur \
	$(RESOURCE_BITMAP_INPUTS) \
	$(MODERN_ICON_RESOURCE_INPUTS)

LINK32_FLAGS=/nologo /subsystem:windows /FORCE:MULTIPLE /incremental:no /debug \
 /LTCG /OPT:REF /OPT:ICF \
 /machine:I386 /nodefaultlib:"libc" \
 /LIBPATH:"$(VCTOOLSINSTALLDIR)ATLMFC\lib\spectre\x86" /LIBPATH:"$(ARTLIB)" \
 /LIBPATH:"$(PORTABLELIB)" \
 uuid.lib comctl32.lib ole32.lib oleaut32.lib windowscodecs.lib oldnames.lib ws2_32.lib \
 libuv.lib mbedtls.lib mbedx509.lib mbedcrypto.lib bcrypt.lib crypt32.lib userenv.lib iphlpapi.lib psapi.lib advapi32.lib \
 shell32.lib winmm.lib imm32.lib winspool.lib comdlg32.lib oledlg.lib wininet.lib zlib.lib \
 /out:"$(OUTDIR)\CChat.exe"

# Standalone console tests deliberately link only their own main() and the
# implementation under test.  In particular, none of the CChat.exe object set
# (and therefore no MFC WinMain) is present in these links.
TEST_LINK32_FLAGS=/nologo /subsystem:console /incremental:no /debug /machine:I386 \
 /LIBPATH:"$(VCTOOLSINSTALLDIR)ATLMFC\lib\spectre\x86" \
 user32.lib gdi32.lib advapi32.lib comctl32.lib ole32.lib shell32.lib

OBJS= \
	"$(INTDIR)\stdafx.obj" \
	"$(INTDIR)\dlylddll.obj" \
	"$(INTDIR)\actions.obj" \
	"$(INTDIR)\admindlg.obj" \
	"$(INTDIR)\arc.obj" \
	"$(INTDIR)\autopage.obj" \
	"$(INTDIR)\avatar.obj" \
	"$(INTDIR)\avatario.obj" \
	"$(INTDIR)\avbfile.obj" \
	"$(INTDIR)\backdrop.obj" \
	"$(INTDIR)\balloon.obj" \
	"$(INTDIR)\bbox.obj" \
	"$(INTDIR)\binddcmt.obj" \
	"$(INTDIR)\binddoc.obj" \
	"$(INTDIR)\bindipfw.obj" \
	"$(INTDIR)\binditem.obj" \
	"$(INTDIR)\bindtarg.obj" \
	"$(INTDIR)\bindview.obj" \
	"$(INTDIR)\bindauto.obj" \
	"$(INTDIR)\bodycam.obj" \
	"$(INTDIR)\ccommon.obj" \
	"$(INTDIR)\ccomp.obj" \
	"$(INTDIR)\chanprop.obj" \
	"$(INTDIR)\chat.obj" \
	"$(INTDIR)\chatbars.obj" \
	"$(INTDIR)\chatDoc.obj" \
	"$(INTDIR)\ChatItem.obj" \
	"$(INTDIR)\chatsrv.obj" \
	"$(INTDIR)\chatView.obj" \
	"$(INTDIR)\chicdial.obj" \
	"$(INTDIR)\childfrm.obj" \
	"$(INTDIR)\colordlg.obj" \
	"$(INTDIR)\coolbar.obj" \
	"$(INTDIR)\dib.obj" \
	"$(INTDIR)\doskey.obj" \
	"$(INTDIR)\filesend.obj" \
	"$(INTDIR)\fonts.obj" \
	"$(INTDIR)\format.obj" \
	"$(INTDIR)\histent.obj" \
	"$(INTDIR)\IpFrame.obj" \
	"$(INTDIR)\ircproto.obj" \
	"$(INTDIR)\ircsock.obj" \
	"$(INTDIR)\crypto_runtime.obj" \
	"$(INTDIR)\memory.obj" \
	"$(INTDIR)\connection_engine.obj" \
	"$(INTDIR)\dcc_transfer_engine.obj" \
	"$(INTDIR)\ircv3.obj" \
	"$(INTDIR)\private_config.obj" \
	"$(INTDIR)\sts_policy_store.obj" \
	"$(INTDIR)\sts_session.obj" \
	"$(INTDIR)\sound_resolver.obj" \
	"$(INTDIR)\transport_adapter_api_compile.obj" \
	"$(INTDIR)\modernicons.obj" \
	"$(INTDIR)\modernui.obj" \
	"$(INTDIR)\MainFrm.obj" \
	"$(INTDIR)\memblst.obj" \
	"$(INTDIR)\mfcbind.obj" \
	"$(INTDIR)\motd.obj" \
	"$(INTDIR)\notif.obj" \
	"$(INTDIR)\notipage.obj" \
	"$(INTDIR)\oleobjct.obj" \
	"$(INTDIR)\PageView.obj" \
	"$(INTDIR)\panel.obj" \
	"$(INTDIR)\print.obj" \
	"$(INTDIR)\proppage.obj" \
	"$(INTDIR)\protsupp.obj" \
	"$(INTDIR)\query.obj" \
	"$(INTDIR)\RoomList.obj" \
	"$(INTDIR)\rtfcmb.obj" \
	"$(INTDIR)\rtfctrl.obj" \
	"$(INTDIR)\rules.obj" \
	"$(INTDIR)\saywnd.obj" \
	"$(INTDIR)\setupdlg.obj" \
	"$(INTDIR)\sounddlg.obj" \
	"$(INTDIR)\spline.obj" \
	"$(INTDIR)\splinutl.obj" \
	"$(INTDIR)\spltchat.obj" \
	"$(INTDIR)\status.obj" \
	"$(INTDIR)\tabbar.obj" \
	"$(INTDIR)\textcore.obj" \
	"$(INTDIR)\textpose.obj" \
	"$(INTDIR)\textview.obj" \
	"$(INTDIR)\traj.obj" \
	"$(INTDIR)\txtfntdg.obj" \
	"$(INTDIR)\urlutil.obj" \
	"$(INTDIR)\userinfo.obj" \
	"$(INTDIR)\userlist.obj" \
	"$(INTDIR)\utils.obj" \
	"$(INTDIR)\vector2d.obj" \
	"$(INTDIR)\webreq.obj" \
	"$(INTDIR)\whisprbx.obj" \
	"$(INTDIR)\jis2sjis.obj" \
	"$(INTDIR)\sjis2jis.obj" \
	"$(INTDIR)\mcithrd.obj" \
	"$(INTDIR)\intl.obj" \
	"$(INTDIR)\icchat_i.obj" \
	"$(INTDIR)\chat.res"

ALL : "$(OUTDIR)\CChat.exe"

TESTS : "$(OUTDIR)\modernui_test.exe" "$(OUTDIR)\transport_ui_bridge_test.exe" "$(OUTDIR)\sts_session_test.exe"

# Keep cleanup restricted to the two configuration directories declared above.
# NMAKE command-line macros override makefile assignments, so recursively
# deleting $(OUTDIR) directly would let an accidental override select an unsafe
# path.  Branch on CFG and use fixed paths instead; this removes the application,
# both standalone tests, their objects, PDBs, and resource output together.
CLEAN :
!IF "$(CFG)" == "chat - Win32 Release"
	@if exist ".\Release\." rmdir /s /q ".\Release"
!ELSE
	@if exist ".\Debug\." rmdir /s /q ".\Debug"
!ENDIF

"$(INTDIR)" :
	if not exist "$(INTDIR)/$(NULL)" mkdir "$(INTDIR)"

# ---- COM proxy from IDL (generates icchat.h + icchat_i.c at root) ----
icchat_i.c icchat.h : base\icchat.idl
	$(MIDL) /nologo /I "$(ARTINC)" /h icchat.h /iid icchat_i.c base\icchat.idl

# ---- Link ----
"$(OUTDIR)\CChat.exe" : "$(INTDIR)" icchat.h $(OBJS)
	$(LINK32) @<<
$(LINK32_FLAGS) $(OBJS)
<<

"$(OUTDIR)\modernui_test.exe" : "$(INTDIR)" "$(INTDIR)\modernui_test.obj" "$(INTDIR)\modernui.obj"
	$(LINK32) @<<
$(TEST_LINK32_FLAGS) /out:"$(OUTDIR)\modernui_test.exe" "$(INTDIR)\modernui_test.obj" "$(INTDIR)\modernui.obj"
<<

"$(OUTDIR)\transport_ui_bridge_test.exe" : "$(INTDIR)" "$(INTDIR)\transport_ui_bridge_test.obj"
	$(LINK32) @<<
$(TEST_LINK32_FLAGS) /out:"$(OUTDIR)\transport_ui_bridge_test.exe" "$(INTDIR)\transport_ui_bridge_test.obj"
<<

"$(OUTDIR)\sts_session_test.exe" : "$(INTDIR)" "$(INTDIR)\sts_session_test.obj" "$(INTDIR)\private_config.obj" "$(INTDIR)\sts_policy_store.obj" "$(INTDIR)\sts_session.obj"
	$(LINK32) @<<
$(TEST_LINK32_FLAGS) /out:"$(OUTDIR)\sts_session_test.exe" "$(INTDIR)\sts_session_test.obj" "$(INTDIR)\private_config.obj" "$(INTDIR)\sts_policy_store.obj" "$(INTDIR)\sts_session.obj"
<<

# ---- Resource ----
"$(INTDIR)\chat.res" : $(RESOURCE_INPUTS)
	$(RSC) $(RSC_PROJ) chat.rc

# ---- C++ sources (no PCH) ----
{.}.cpp{$(INTDIR)}.obj:
	$(CPP) $(CPP_PROJ) $<

"$(INTDIR)\crypto_runtime.obj" : ..\portable\src\crypto_runtime.cpp
	$(CPP) $(CPP_PROJ) /Fo"$(INTDIR)\crypto_runtime.obj" ..\portable\src\crypto_runtime.cpp

"$(INTDIR)\memory.obj" : ..\portable\src\memory.cpp
	$(CPP) $(CPP_PROJ) /Fo"$(INTDIR)\memory.obj" ..\portable\src\memory.cpp

"$(INTDIR)\connection_engine.obj" : ..\portable\src\net\connection_engine.cpp
	$(CPP) $(CPP_PROJ) /Fo"$(INTDIR)\connection_engine.obj" ..\portable\src\net\connection_engine.cpp

"$(INTDIR)\dcc_transfer_engine.obj" : ..\portable\src\net\dcc_transfer_engine.cpp
	$(CPP) $(CPP_PROJ) /Fo"$(INTDIR)\dcc_transfer_engine.obj" ..\portable\src\net\dcc_transfer_engine.cpp

"$(INTDIR)\ircv3.obj" : ..\portable\src\net\ircv3.cpp
	$(CPP) $(CPP_PROJ) /Fo"$(INTDIR)\ircv3.obj" ..\portable\src\net\ircv3.cpp

"$(INTDIR)\private_config.obj" : ..\portable\src\net\private_config.cpp
	$(CPP) $(CPP_PROJ) /Fo"$(INTDIR)\private_config.obj" ..\portable\src\net\private_config.cpp

"$(INTDIR)\sts_policy_store.obj" : ..\portable\src\net\sts_policy_store.cpp
	$(CPP) $(CPP_PROJ) /Fo"$(INTDIR)\sts_policy_store.obj" ..\portable\src\net\sts_policy_store.cpp

"$(INTDIR)\sts_session.obj" : ..\portable\src\net\sts_session.cpp
	$(CPP) $(CPP_PROJ) /Fo"$(INTDIR)\sts_session.obj" ..\portable\src\net\sts_session.cpp

"$(INTDIR)\sound_resolver.obj" : ..\portable\src\sound.cpp
	$(CPP) $(CPP_PROJ) /Fo"$(INTDIR)\sound_resolver.obj" ..\portable\src\sound.cpp

"$(INTDIR)\transport_adapter_api_compile.obj" : tests\transport_adapter_api_compile.cpp
	$(CPP) $(CPP_PROJ) /Fo"$(INTDIR)\transport_adapter_api_compile.obj" tests\transport_adapter_api_compile.cpp

"$(INTDIR)\modernui_test.obj" : tests\modernui_test.cpp
	$(CPP) $(CPP_PROJ) /Fo"$(INTDIR)\modernui_test.obj" tests\modernui_test.cpp

"$(INTDIR)\transport_ui_bridge_test.obj" : tests\transport_ui_bridge_test.cpp
	$(CPP) $(CPP_PROJ) /Fo"$(INTDIR)\transport_ui_bridge_test.obj" tests\transport_ui_bridge_test.cpp

"$(INTDIR)\sts_session_test.obj" : ..\portable\tests\sts_session_test.cpp
	$(CPP) $(CPP_PROJ) /Fo"$(INTDIR)\sts_session_test.obj" ..\portable\tests\sts_session_test.cpp

# ---- C sources ----
{.}.c{$(INTDIR)}.obj:
	$(CPP) $(C_PROJ) $<
