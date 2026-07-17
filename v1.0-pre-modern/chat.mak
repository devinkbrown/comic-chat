# Modern NMAKE build for the Microsoft Comic Chat 1.0 prerelease client.
#
# The source inventory and x86 MFC/resource shape come from the original
# Developer Studio 4.2 project.  Current MSVC compiles every product C++
# translation unit in its post-C++23 working-draft mode; generated or
# third-party C remains C.  The shared libuv/mbedTLS transport is linked here
# as the exclusive runtime transport used by the plain v1 adapter.

# TARGTYPE "Win32 (x86) Application" 0x0101

!IF "$(CFG)" == ""
CFG=chat - Win32 Debug
!MESSAGE No configuration specified.  Defaulting to chat - Win32 Debug.
!ENDIF 

!IF "$(CFG)" != "chat - Win32 Release" && "$(CFG)" != "chat - Win32 Debug"
!MESSAGE Invalid configuration "$(CFG)" specified.
!MESSAGE You can specify a configuration when running NMAKE on this makefile
!MESSAGE by defining the macro CFG on the command line.  For example:
!MESSAGE 
!MESSAGE NMAKE /f "chat.mak" CFG="chat - Win32 Debug"
!MESSAGE 
!MESSAGE Possible choices for configuration are:
!MESSAGE 
!MESSAGE "chat - Win32 Release" (based on "Win32 (x86) Application")
!MESSAGE "chat - Win32 Debug" (based on "Win32 (x86) Application")
!MESSAGE 
!ERROR An invalid configuration is specified.
!ENDIF 

!IF "$(OS)" == "Windows_NT"
NULL=
!ELSE 
NULL=nul
!ENDIF 
################################################################################
# Begin Project
# PROP Target_Last_Scanned "chat - Win32 Debug"
RSC=rc.exe
MTL=mktyplib.exe
CPP=cl.exe

PORTABLE_LIB=..\artifacts\lib

# There is no /std:c++26 spelling in current MSVC.  /std:c++latest is the
# supported post-C++23 working-draft mode and cpp26mode.h makes that setting,
# the current production toolset, and the conforming preprocessor hard build
# invariants for every C++ compile recipe in this makefile.
CPP26_FLAGS=/std:c++latest /permissive- /EHsc /Zc:__cplusplus /Zc:preprocessor \
 /Zc:forScope /Zc:strictStrings /Zc:wchar_t /Zc:inline /Zc:externConstexpr \
 /Zc:lambda /Zc:twoPhase /Zc:throwingNew /Zc:ternary /volatile:iso

CPP26_COMMON=$(CPP26_FLAGS) /FI"cpp26mode.h" \
 /D "WIN32" /D "_WINDOWS" /D "_MBCS" \
 /D "MBEDTLS_USER_CONFIG_FILE=\"comicchat/mbedtls_user_config.h\""

# C_PROJ deliberately omits the C++ language-mode and force-include flags.
# It is reserved for generated/third-party .c translation units.
C_COMMON=/D "WIN32" /D "_WINDOWS" /D "_MBCS" \
 /D "MBEDTLS_USER_CONFIG_FILE=\"comicchat/mbedtls_user_config.h\""

!IF  "$(CFG)" == "chat - Win32 Release"

# PROP BASE Use_MFC 6
# PROP BASE Use_Debug_Libraries 0
# PROP BASE Output_Dir "Release"
# PROP BASE Intermediate_Dir "Release"
# PROP BASE Target_Dir ""
# PROP Use_MFC 5
# PROP Use_Debug_Libraries 0
# PROP Output_Dir ""
# PROP Intermediate_Dir "Release"
# PROP Target_Dir ""
OUTDIR=.
INTDIR=.\Release

ALL : "$(OUTDIR)\chat.exe"

CLEAN : 
	@if exist ".\Release\." rmdir /s /q ".\Release"
	-@erase ".\chat.exe"
	-@erase ".\chat.pdb"

"$(INTDIR)" :
    if not exist "$(INTDIR)/$(NULL)" mkdir "$(INTDIR)"

# PCH consumption is disabled; chat.pch remains a dependency stamp for the
# generated per-source graph so cpp26mode.h changes invalidate every v1 object.
CPP_PROJ=/nologo /MT /W4 /Zi /O2 /GL /Gy /Gw $(CPP26_COMMON) /D "NDEBUG" \
 /I "." /I "..\portable\include" /I "..\third_party\libuv\include" \
 /I "..\third_party\mbedtls\include" \
 /Fp"$(INTDIR)/chat.pch" /Fo"$(INTDIR)/" /Fd"$(INTDIR)/" /c
C_PROJ=/nologo /MT /W3 /Zi /O2 /GL /Gy /Gw $(C_COMMON) /D "NDEBUG" \
 /I "." /I "..\portable\include" /I "..\third_party\libuv\include" \
 /I "..\third_party\mbedtls\include" \
 /Fo"$(INTDIR)/" /Fd"$(INTDIR)/" /c
CPP_OBJS=.\Release/
CPP_SBRS=.\.
# ADD BASE MTL /nologo /D "NDEBUG" /win32
# ADD MTL /nologo /D "NDEBUG" /win32
MTL_PROJ=/nologo /D "NDEBUG" /win32 
# ADD BASE RSC /l 0x409 /d "NDEBUG" /d "_AFXDLL"
# ADD RSC /l 0x409 /d "NDEBUG"
RSC_PROJ=/l 0x409 /fo"$(INTDIR)/chat.res" /d "NDEBUG" 
BSC32=bscmake.exe
# ADD BASE BSC32 /nologo
# ADD BSC32 /nologo
BSC32_FLAGS=/nologo /o"$(OUTDIR)/chat.bsc" 
BSC32_SBRS= \
	
LINK32=link.exe
# ADD BASE LINK32 /nologo /subsystem:windows /machine:I386
# ADD LINK32 uuid.lib winmm.lib /nologo /subsystem:windows /machine:I386 /nodefaultlib:"libc"
LINK32_FLAGS=uuid.lib ole32.lib shell32.lib winmm.lib ws2_32.lib libuv.lib mbedtls.lib mbedx509.lib \
 mbedcrypto.lib bcrypt.lib crypt32.lib userenv.lib iphlpapi.lib psapi.lib advapi32.lib \
 /nologo /subsystem:windows /incremental:no /FORCE:MULTIPLE /LTCG /OPT:REF /OPT:ICF\
 /LIBPATH:"$(VCTOOLSINSTALLDIR)ATLMFC\lib\spectre\x86"\
 /LIBPATH:"$(PORTABLE_LIB)"\
 /pdb:"$(OUTDIR)/chat.pdb" /machine:I386 /nodefaultlib:"libc"\
 /out:"$(OUTDIR)/chat.exe" 
LINK32_OBJS= \
	"$(INTDIR)\admindlg.obj" \
	"$(INTDIR)\arc.obj" \
	"$(INTDIR)\avatar.obj" \
	"$(INTDIR)\avatardl.obj" \
	"$(INTDIR)\avatario.obj" \
	"$(INTDIR)\backdrop.obj" \
	"$(INTDIR)\balloon.obj" \
	"$(INTDIR)\bbox.obj" \
	"$(INTDIR)\binddcmt.obj" \
	"$(INTDIR)\binddoc.obj" \
	"$(INTDIR)\bindipfw.obj" \
	"$(INTDIR)\binditem.obj" \
	"$(INTDIR)\bindtarg.obj" \
	"$(INTDIR)\bindview.obj" \
	"$(INTDIR)\bodycam.obj" \
	"$(INTDIR)\chat.obj" \
	"$(INTDIR)\chat.res" \
	"$(INTDIR)\chatDoc.obj" \
	"$(INTDIR)\ChatItem.obj" \
	"$(INTDIR)\chatprot.obj" \
	"$(INTDIR)\chatView.obj" \
	"$(INTDIR)\Dib.obj" \
	"$(INTDIR)\fonts.obj" \
	"$(INTDIR)\histent.obj" \
	"$(INTDIR)\IpFrame.obj" \
	"$(INTDIR)\irc.obj" \
	"$(INTDIR)\crypto_runtime.obj" \
	"$(INTDIR)\memory.obj" \
	"$(INTDIR)\connection_engine.obj" \
	"$(INTDIR)\dcc_transfer_engine.obj" \
	"$(INTDIR)\ircv3.obj" \
	"$(INTDIR)\private_config.obj" \
	"$(INTDIR)\sts_policy_store.obj" \
	"$(INTDIR)\sts_session.obj" \
	"$(INTDIR)\transport_adapter_api_compile.obj" \
	"$(INTDIR)\MainFrm.obj" \
	"$(INTDIR)\memblst.obj" \
	"$(INTDIR)\mfcbind.obj" \
	"$(INTDIR)\oleobjct.obj" \
	"$(INTDIR)\PageView.obj" \
	"$(INTDIR)\panel.obj" \
	"$(INTDIR)\print.obj" \
	"$(INTDIR)\profdlg.obj" \
	"$(INTDIR)\proppage.obj" \
	"$(INTDIR)\roomlist.obj" \
	"$(INTDIR)\saywnd.obj" \
	"$(INTDIR)\script.obj" \
	"$(INTDIR)\semantic.obj" \
	"$(INTDIR)\setupdlg.obj" \
	"$(INTDIR)\spline.obj" \
	"$(INTDIR)\splinutl.obj" \
	"$(INTDIR)\spltchat.obj" \
	"$(INTDIR)\StdAfx.obj" \
	"$(INTDIR)\textpose.obj" \
	"$(INTDIR)\textview.obj" \
	"$(INTDIR)\traj.obj" \
	"$(INTDIR)\ui.obj" \
	"$(INTDIR)\url.obj" \
	"$(INTDIR)\vector2d.obj"

"$(OUTDIR)\chat.exe" : "$(OUTDIR)" $(DEF_FILE) $(LINK32_OBJS)
    $(LINK32) @<<
  $(LINK32_FLAGS) $(LINK32_OBJS)
<<

!ELSEIF  "$(CFG)" == "chat - Win32 Debug"

# PROP BASE Use_MFC 6
# PROP BASE Use_Debug_Libraries 1
# PROP BASE Output_Dir "Debug"
# PROP BASE Intermediate_Dir "Debug"
# PROP BASE Target_Dir ""
# PROP Use_MFC 5
# PROP Use_Debug_Libraries 1
# PROP Output_Dir ""
# PROP Intermediate_Dir "Debug"
# PROP Target_Dir ""
OUTDIR=.
INTDIR=.\Debug

ALL : "$(OUTDIR)\chat.exe"

CLEAN : 
	@if exist ".\Debug\." rmdir /s /q ".\Debug"
	-@erase ".\chat.exe"
	-@erase ".\chat.pdb"

"$(INTDIR)" :
    if not exist "$(INTDIR)/$(NULL)" mkdir "$(INTDIR)"

# Keep the same strict language/conformance surface in Debug and Release.
CPP_PROJ=/nologo /MTd /W4 /Zi /Od $(CPP26_COMMON) /D "_DEBUG" \
 /I "." /I "..\portable\include" /I "..\third_party\libuv\include" \
 /I "..\third_party\mbedtls\include" \
 /Fp"$(INTDIR)/chat.pch" /Fo"$(INTDIR)/" /Fd"$(INTDIR)/" /c
C_PROJ=/nologo /MTd /W3 /Zi /Od $(C_COMMON) /D "_DEBUG" \
 /I "." /I "..\portable\include" /I "..\third_party\libuv\include" \
 /I "..\third_party\mbedtls\include" \
 /Fo"$(INTDIR)/" /Fd"$(INTDIR)/" /c
CPP_OBJS=.\Debug/
CPP_SBRS=.\.
# ADD BASE MTL /nologo /D "_DEBUG" /win32
# ADD MTL /nologo /D "_DEBUG" /win32
MTL_PROJ=/nologo /D "_DEBUG" /win32 
# ADD BASE RSC /l 0x409 /d "_DEBUG" /d "_AFXDLL"
# ADD RSC /l 0x409 /d "_DEBUG"
RSC_PROJ=/l 0x409 /fo"$(INTDIR)/chat.res" /d "_DEBUG" 
BSC32=bscmake.exe
# ADD BASE BSC32 /nologo
# ADD BSC32 /nologo
BSC32_FLAGS=/nologo /o"$(OUTDIR)/chat.bsc" 
BSC32_SBRS= \
	
LINK32=link.exe
# ADD BASE LINK32 /nologo /subsystem:windows /debug /machine:I386
# ADD LINK32 uuid.lib winmm.lib /nologo /subsystem:windows /incremental:no /debug /machine:I386 /nodefaultlib:"libc"
LINK32_FLAGS=uuid.lib ole32.lib shell32.lib winmm.lib ws2_32.lib libuv.lib mbedtls.lib mbedx509.lib \
 mbedcrypto.lib bcrypt.lib crypt32.lib userenv.lib iphlpapi.lib psapi.lib advapi32.lib \
 /nologo /subsystem:windows /FORCE:MULTIPLE\
 /LIBPATH:"$(VCTOOLSINSTALLDIR)ATLMFC\lib\spectre\x86"\
 /LIBPATH:"$(PORTABLE_LIB)"\
 /incremental:no /pdb:"$(OUTDIR)/chat.pdb" /debug /machine:I386\
 /nodefaultlib:"libc" /out:"$(OUTDIR)/chat.exe" 
LINK32_OBJS= \
	"$(INTDIR)\admindlg.obj" \
	"$(INTDIR)\arc.obj" \
	"$(INTDIR)\avatar.obj" \
	"$(INTDIR)\avatardl.obj" \
	"$(INTDIR)\avatario.obj" \
	"$(INTDIR)\backdrop.obj" \
	"$(INTDIR)\balloon.obj" \
	"$(INTDIR)\bbox.obj" \
	"$(INTDIR)\binddcmt.obj" \
	"$(INTDIR)\binddoc.obj" \
	"$(INTDIR)\bindipfw.obj" \
	"$(INTDIR)\binditem.obj" \
	"$(INTDIR)\bindtarg.obj" \
	"$(INTDIR)\bindview.obj" \
	"$(INTDIR)\bodycam.obj" \
	"$(INTDIR)\chat.obj" \
	"$(INTDIR)\chat.res" \
	"$(INTDIR)\chatDoc.obj" \
	"$(INTDIR)\ChatItem.obj" \
	"$(INTDIR)\chatprot.obj" \
	"$(INTDIR)\chatView.obj" \
	"$(INTDIR)\Dib.obj" \
	"$(INTDIR)\fonts.obj" \
	"$(INTDIR)\histent.obj" \
	"$(INTDIR)\IpFrame.obj" \
	"$(INTDIR)\irc.obj" \
	"$(INTDIR)\crypto_runtime.obj" \
	"$(INTDIR)\memory.obj" \
	"$(INTDIR)\connection_engine.obj" \
	"$(INTDIR)\dcc_transfer_engine.obj" \
	"$(INTDIR)\ircv3.obj" \
	"$(INTDIR)\private_config.obj" \
	"$(INTDIR)\sts_policy_store.obj" \
	"$(INTDIR)\sts_session.obj" \
	"$(INTDIR)\transport_adapter_api_compile.obj" \
	"$(INTDIR)\MainFrm.obj" \
	"$(INTDIR)\memblst.obj" \
	"$(INTDIR)\mfcbind.obj" \
	"$(INTDIR)\oleobjct.obj" \
	"$(INTDIR)\PageView.obj" \
	"$(INTDIR)\panel.obj" \
	"$(INTDIR)\print.obj" \
	"$(INTDIR)\profdlg.obj" \
	"$(INTDIR)\proppage.obj" \
	"$(INTDIR)\roomlist.obj" \
	"$(INTDIR)\saywnd.obj" \
	"$(INTDIR)\script.obj" \
	"$(INTDIR)\semantic.obj" \
	"$(INTDIR)\setupdlg.obj" \
	"$(INTDIR)\spline.obj" \
	"$(INTDIR)\splinutl.obj" \
	"$(INTDIR)\spltchat.obj" \
	"$(INTDIR)\StdAfx.obj" \
	"$(INTDIR)\textpose.obj" \
	"$(INTDIR)\textview.obj" \
	"$(INTDIR)\traj.obj" \
	"$(INTDIR)\ui.obj" \
	"$(INTDIR)\url.obj" \
	"$(INTDIR)\vector2d.obj"

"$(OUTDIR)\chat.exe" : "$(OUTDIR)" $(DEF_FILE) $(LINK32_OBJS)
    $(LINK32) @<<
  $(LINK32_FLAGS) $(LINK32_OBJS)
<<

!ENDIF 

.c{$(CPP_OBJS)}.obj:
   $(CPP) $(C_PROJ) $<

.cpp{$(CPP_OBJS)}.obj:
   $(CPP) $(CPP_PROJ) $<  

.cxx{$(CPP_OBJS)}.obj:
   $(CPP) $(CPP_PROJ) $<  

.c{$(CPP_SBRS)}.sbr:
   $(CPP) $(C_PROJ) $<

.cpp{$(CPP_SBRS)}.sbr:
   $(CPP) $(CPP_PROJ) $<  

.cxx{$(CPP_SBRS)}.sbr:
   $(CPP) $(CPP_PROJ) $<  

# Shared runtime transport. API, ABI, and native dependency drift fail the v1
# build; CIrcSocket delegates all IRC ownership to these objects.
"$(INTDIR)\crypto_runtime.obj" : ..\portable\src\crypto_runtime.cpp cpp26mode.h "$(INTDIR)"
	$(CPP) $(CPP_PROJ) /Fo"$(INTDIR)\crypto_runtime.obj" ..\portable\src\crypto_runtime.cpp

"$(INTDIR)\memory.obj" : ..\portable\src\memory.cpp cpp26mode.h "$(INTDIR)"
	$(CPP) $(CPP_PROJ) /Fo"$(INTDIR)\memory.obj" ..\portable\src\memory.cpp

"$(INTDIR)\connection_engine.obj" : ..\portable\src\net\connection_engine.cpp cpp26mode.h "$(INTDIR)"
	$(CPP) $(CPP_PROJ) /Fo"$(INTDIR)\connection_engine.obj" ..\portable\src\net\connection_engine.cpp

"$(INTDIR)\dcc_transfer_engine.obj" : ..\portable\src\net\dcc_transfer_engine.cpp cpp26mode.h "$(INTDIR)"
	$(CPP) $(CPP_PROJ) /Fo"$(INTDIR)\dcc_transfer_engine.obj" ..\portable\src\net\dcc_transfer_engine.cpp

"$(INTDIR)\ircv3.obj" : ..\portable\src\net\ircv3.cpp cpp26mode.h "$(INTDIR)"
	$(CPP) $(CPP_PROJ) /Fo"$(INTDIR)\ircv3.obj" ..\portable\src\net\ircv3.cpp

"$(INTDIR)\private_config.obj" : ..\portable\src\net\private_config.cpp cpp26mode.h "$(INTDIR)"
	$(CPP) $(CPP_PROJ) /Fo"$(INTDIR)\private_config.obj" ..\portable\src\net\private_config.cpp

"$(INTDIR)\sts_policy_store.obj" : ..\portable\src\net\sts_policy_store.cpp cpp26mode.h "$(INTDIR)"
	$(CPP) $(CPP_PROJ) /Fo"$(INTDIR)\sts_policy_store.obj" ..\portable\src\net\sts_policy_store.cpp

"$(INTDIR)\sts_session.obj" : ..\portable\src\net\sts_session.cpp cpp26mode.h "$(INTDIR)"
	$(CPP) $(CPP_PROJ) /Fo"$(INTDIR)\sts_session.obj" ..\portable\src\net\sts_session.cpp

"$(INTDIR)\transport_adapter_api_compile.obj" : tests\transport_adapter_api_compile.cpp cpp26mode.h transportadapter.h "$(INTDIR)"
	$(CPP) $(CPP_PROJ) /Fo"$(INTDIR)\transport_adapter_api_compile.obj" tests\transport_adapter_api_compile.cpp

################################################################################
# Begin Target

# Name "chat - Win32 Release"
# Name "chat - Win32 Debug"

!IF  "$(CFG)" == "chat - Win32 Release"

!ELSEIF  "$(CFG)" == "chat - Win32 Debug"

!ENDIF 

################################################################################
# Begin Source File

SOURCE=.\ReadMe.txt

!IF  "$(CFG)" == "chat - Win32 Release"

!ELSEIF  "$(CFG)" == "chat - Win32 Debug"

!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\chat.cpp
DEP_CPP_CHAT_=\
	".\binddoc.h"\
	".\bindipfw.h"\
	".\chat.h"\
	".\chatDoc.h"\
	".\chatprot.h"\
	".\chatView.h"\
	".\IpFrame.h"\
	".\MainFrm.h"\
	".\mfcbind.h"\
	".\spltchat.h"\
	".\StdAfx.h"\
	".\ui.h"\
	".\version.h"\
	

"$(INTDIR)\chat.obj" : $(SOURCE) $(DEP_CPP_CHAT_) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\StdAfx.cpp
DEP_CPP_STDAF=\
	".\StdAfx.h"\
	

!IF  "$(CFG)" == "chat - Win32 Release"

# ADD CPP /Yc"stdafx.h"

BuildCmds= \
	$(CPP) $(CPP_PROJ) /Yc"stdafx.h" $(SOURCE) \
	

"$(INTDIR)\StdAfx.obj" : $(SOURCE) $(DEP_CPP_STDAF) cpp26mode.h "$(INTDIR)"
   $(BuildCmds)

"$(INTDIR)\chat.pch" : $(SOURCE) $(DEP_CPP_STDAF) cpp26mode.h "$(INTDIR)"
   $(BuildCmds)

!ELSEIF  "$(CFG)" == "chat - Win32 Debug"

# ADD CPP /Yc"stdafx.h"

BuildCmds= \
	$(CPP) $(CPP_PROJ) /Yc"stdafx.h" $(SOURCE) \
	

"$(INTDIR)\StdAfx.obj" : $(SOURCE) $(DEP_CPP_STDAF) cpp26mode.h "$(INTDIR)"
   $(BuildCmds)

"$(INTDIR)\chat.pch" : $(SOURCE) $(DEP_CPP_STDAF) cpp26mode.h "$(INTDIR)"
   $(BuildCmds)

!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\MainFrm.cpp
DEP_CPP_MAINF=\
	".\avatar.h"\
	".\bbox.h"\
	".\bodycam.h"\
	".\chat.h"\
	".\dib.h"\
	".\ircsock.h"\
	".\MainFrm.h"\
	".\memblst.h"\
	".\pe.h"\
	".\StdAfx.h"\
	".\ui.h"\
	

"$(INTDIR)\MainFrm.obj" : $(SOURCE) $(DEP_CPP_MAINF) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\chatDoc.cpp
DEP_CPP_CHATD=\
	".\avatar.h"\
	".\avatardl.h"\
	".\backdrop.h"\
	".\balloon.h"\
	".\bbox.h"\
	".\binddoc.h"\
	".\binditem.h"\
	".\bodycam.h"\
	".\chat.h"\
	".\chatDoc.h"\
	".\ChatItem.h"\
	".\chatprot.h"\
	".\chatView.h"\
	".\dib.h"\
	".\histent.h"\
	".\memblst.h"\
	".\PageView.h"\
	".\panel.h"\
	".\pe.h"\
	".\profdlg.h"\
	".\proppage.h"\
	".\roomlist.h"\
	".\saywnd.h"\
	".\script.h"\
	".\setupdlg.h"\
	".\spline.h"\
	".\spltchat.h"\
	".\StdAfx.h"\
	".\textview.h"\
	".\traj.h"\
	".\ui.h"\
	".\userinfo.h"\
	

"$(INTDIR)\chatDoc.obj" : $(SOURCE) $(DEP_CPP_CHATD) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\chatView.cpp
DEP_CPP_CHATV=\
	".\avatar.h"\
	".\avatardl.h"\
	".\bbox.h"\
	".\binddoc.h"\
	".\bodycam.h"\
	".\chat.h"\
	".\chatDoc.h"\
	".\chatView.h"\
	".\dib.h"\
	".\DumbWnd.h"\
	".\memblst.h"\
	".\PageView.h"\
	".\pe.h"\
	".\saywnd.h"\
	".\script.h"\
	".\spltchat.h"\
	".\StdAfx.h"\
	".\textview.h"\
	".\ui.h"\
	".\userinfo.h"\
	

"$(INTDIR)\chatView.obj" : $(SOURCE) $(DEP_CPP_CHATV) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\ChatItem.cpp
DEP_CPP_CHATI=\
	".\binddoc.h"\
	".\binditem.h"\
	".\chat.h"\
	".\chatDoc.h"\
	".\ChatItem.h"\
	".\StdAfx.h"\
	

"$(INTDIR)\ChatItem.obj" : $(SOURCE) $(DEP_CPP_CHATI) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\IpFrame.cpp
DEP_CPP_IPFRA=\
	".\bindipfw.h"\
	".\chat.h"\
	".\IpFrame.h"\
	".\memblst.h"\
	".\StdAfx.h"\
	".\ui.h"\
	

"$(INTDIR)\IpFrame.obj" : $(SOURCE) $(DEP_CPP_IPFRA) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\chat.rc
DEP_RSC_CHAT_R=\
	".\res\balloons.bmp"\
	".\res\bitmap1.bmp"\
	".\res\chat.ico"\
	".\res\chat.rc2"\
	".\res\chatDoc.ico"\
	".\res\IToolbar.bmp"\
	".\res\Toolbar.bmp"\
	

"$(INTDIR)\chat.res" : $(SOURCE) $(DEP_RSC_CHAT_R) "$(INTDIR)"
   $(RSC) $(RSC_PROJ) $(SOURCE)


# End Source File
################################################################################
# Begin Source File

SOURCE=.\oleobjct.cpp
DEP_CPP_OLEOB=\
	".\binddoc.h"\
	".\StdAfx.h"\
	

"$(INTDIR)\oleobjct.obj" : $(SOURCE) $(DEP_CPP_OLEOB) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\mfcbind.cpp
DEP_CPP_MFCBI=\
	".\mfcbind.h"\
	".\StdAfx.h"\
	

"$(INTDIR)\mfcbind.obj" : $(SOURCE) $(DEP_CPP_MFCBI) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\bindview.cpp
DEP_CPP_BINDV=\
	".\binddoc.h"\
	".\bindipfw.h"\
	".\StdAfx.h"\
	

"$(INTDIR)\bindview.obj" : $(SOURCE) $(DEP_CPP_BINDV) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\bindtarg.cpp
DEP_CPP_BINDT=\
	".\binddoc.h"\
	".\StdAfx.h"\
	

"$(INTDIR)\bindtarg.obj" : $(SOURCE) $(DEP_CPP_BINDT) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\binditem.cpp
DEP_CPP_BINDI=\
	".\binddoc.h"\
	".\binditem.h"\
	".\StdAfx.h"\
	

"$(INTDIR)\binditem.obj" : $(SOURCE) $(DEP_CPP_BINDI) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\binddoc.cpp
DEP_CPP_BINDD=\
	".\binddoc.h"\
	".\StdAfx.h"\
	".\ui.h"\
	

"$(INTDIR)\binddoc.obj" : $(SOURCE) $(DEP_CPP_BINDD) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\bindipfw.cpp
DEP_CPP_BINDIP=\
	".\binddoc.h"\
	".\bindipfw.h"\
	".\mfcbind.h"\
	".\StdAfx.h"\
	

"$(INTDIR)\bindipfw.obj" : $(SOURCE) $(DEP_CPP_BINDIP) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\binddcmt.cpp
DEP_CPP_BINDDC=\
	".\binddoc.h"\
	".\StdAfx.h"\
	

"$(INTDIR)\binddcmt.obj" : $(SOURCE) $(DEP_CPP_BINDDC) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\print.cpp
DEP_CPP_PRINT=\
	".\binddoc.h"\
	".\StdAfx.h"\
	

"$(INTDIR)\print.obj" : $(SOURCE) $(DEP_CPP_PRINT) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\PageView.cpp
DEP_CPP_PAGEV=\
	".\avatar.h"\
	".\backdrop.h"\
	".\balloon.h"\
	".\bbox.h"\
	".\binddoc.h"\
	".\bodycam.h"\
	".\chat.h"\
	".\chatDoc.h"\
	".\chatprot.h"\
	".\common.h"\
	".\dib.h"\
	".\histent.h"\
	".\MainFrm.h"\
	".\PageView.h"\
	".\panel.h"\
	".\pe.h"\
	".\saywnd.h"\
	".\script.h"\
	".\spline.h"\
	".\StdAfx.h"\
	".\traj.h"\
	".\ui.h"\
	".\userinfo.h"\
	

"$(INTDIR)\PageView.obj" : $(SOURCE) $(DEP_CPP_PAGEV) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\saywnd.cpp
DEP_CPP_SAYWN=\
	".\binddoc.h"\
	".\bothdlg.h"\
	".\chat.h"\
	".\chatDoc.h"\
	".\chatprot.h"\
	".\dib.h"\
	".\PageView.h"\
	".\saywnd.h"\
	".\script.h"\
	".\StdAfx.h"\
	".\ui.h"\
	

"$(INTDIR)\saywnd.obj" : $(SOURCE) $(DEP_CPP_SAYWN) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\ui.cpp
DEP_CPP_UI_CP=\
	".\chat.h"\
	".\chatprot.h"\
	".\MainFrm.h"\
	".\StdAfx.h"\
	".\ui.h"\
	

"$(INTDIR)\ui.obj" : $(SOURCE) $(DEP_CPP_UI_CP) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\textpose.cpp
DEP_CPP_TEXTP=\
	".\avatar.h"\
	".\bbox.h"\
	".\bodycam.h"\
	".\chat.h"\
	".\dib.h"\
	".\pe.h"\
	".\StdAfx.h"\
	".\ui.h"\
	".\vector2d.h"\
	

"$(INTDIR)\textpose.obj" : $(SOURCE) $(DEP_CPP_TEXTP) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\irc.cpp
DEP_CPP_IRC_C=\
	".\admindlg.h"\
	".\avatar.h"\
	".\bbox.h"\
	".\binddoc.h"\
	".\chat.h"\
	".\chatDoc.h"\
	".\chatprot.h"\
	".\dib.h"\
	".\histent.h"\
	".\ircsock.h"\
	".\transportadapter.h"\
	".\memblst.h"\
	".\pe.h"\
	".\roomlist.h"\
	".\setupdlg.h"\
	".\StdAfx.h"\
	".\ui.h"\
	".\userinfo.h"\
	

"$(INTDIR)\irc.obj" : $(SOURCE) $(DEP_CPP_IRC_C) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\Dib.cpp
DEP_CPP_DIB_C=\
	".\dib.h"\
	".\StdAfx.h"\
	

"$(INTDIR)\Dib.obj" : $(SOURCE) $(DEP_CPP_DIB_C) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\bbox.cpp
DEP_CPP_BBOX_=\
	".\bbox.h"\
	".\StdAfx.h"\
	".\vector2d.h"\
	

"$(INTDIR)\bbox.obj" : $(SOURCE) $(DEP_CPP_BBOX_) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\vector2d.cpp
DEP_CPP_VECTO=\
	".\StdAfx.h"\
	".\vector2d.h"\
	

"$(INTDIR)\vector2d.obj" : $(SOURCE) $(DEP_CPP_VECTO) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\avatar.cpp
DEP_CPP_AVATA=\
	".\avatar.h"\
	".\bbox.h"\
	".\chat.h"\
	".\chatprot.h"\
	".\dib.h"\
	".\memblst.h"\
	".\pe.h"\
	".\StdAfx.h"\
	".\ui.h"\
	".\userinfo.h"\
	".\vector2d.h"\
	

"$(INTDIR)\avatar.obj" : $(SOURCE) $(DEP_CPP_AVATA) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\bodycam.cpp
DEP_CPP_BODYC=\
	".\avatar.h"\
	".\bbox.h"\
	".\binddoc.h"\
	".\bodycam.h"\
	".\chat.h"\
	".\chatDoc.h"\
	".\chatprot.h"\
	".\common.h"\
	".\dib.h"\
	".\pe.h"\
	".\saywnd.h"\
	".\StdAfx.h"\
	".\ui.h"\
	".\vector2d.h"\
	

"$(INTDIR)\bodycam.obj" : $(SOURCE) $(DEP_CPP_BODYC) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\script.cpp
DEP_CPP_SCRIP=\
	".\script.h"\
	".\StdAfx.h"\
	

"$(INTDIR)\script.obj" : $(SOURCE) $(DEP_CPP_SCRIP) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\panel.cpp
DEP_CPP_PANEL=\
	".\avatar.h"\
	".\backdrop.h"\
	".\balloon.h"\
	".\bbox.h"\
	".\binddoc.h"\
	".\chat.h"\
	".\chatDoc.h"\
	".\chatprot.h"\
	".\common.h"\
	".\dib.h"\
	".\panel.h"\
	".\pe.h"\
	".\spline.h"\
	".\StdAfx.h"\
	".\traj.h"\
	".\ui.h"\
	".\userinfo.h"\
	".\vector2d.h"\
	

"$(INTDIR)\panel.obj" : $(SOURCE) $(DEP_CPP_PANEL) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\traj.cpp
DEP_CPP_TRAJ_=\
	".\StdAfx.h"\
	".\traj.h"\
	".\vector2d.h"\
	

"$(INTDIR)\traj.obj" : $(SOURCE) $(DEP_CPP_TRAJ_) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\spline.cpp
DEP_CPP_SPLIN=\
	".\spline.h"\
	".\StdAfx.h"\
	".\traj.h"\
	".\vector2d.h"\
	

"$(INTDIR)\spline.obj" : $(SOURCE) $(DEP_CPP_SPLIN) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\balloon.cpp
DEP_CPP_BALLO=\
	".\avatar.h"\
	".\backdrop.h"\
	".\balloon.h"\
	".\bbox.h"\
	".\chat.h"\
	".\chatprot.h"\
	".\common.h"\
	".\dib.h"\
	".\panel.h"\
	".\pe.h"\
	".\spline.h"\
	".\StdAfx.h"\
	".\traj.h"\
	".\ui.h"\
	".\url.h"\
	".\vector2d.h"\
	

"$(INTDIR)\balloon.obj" : $(SOURCE) $(DEP_CPP_BALLO) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\backdrop.cpp
DEP_CPP_BACKD=\
	".\backdrop.h"\
	".\bbox.h"\
	".\chat.h"\
	".\dib.h"\
	".\histent.h"\
	".\pe.h"\
	".\StdAfx.h"\
	".\userinfo.h"\
	".\vector2d.h"\
	

"$(INTDIR)\backdrop.obj" : $(SOURCE) $(DEP_CPP_BACKD) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\arc.cpp
DEP_CPP_ARC_C=\
	".\StdAfx.h"\
	".\traj.h"\
	".\vector2d.h"\
	

"$(INTDIR)\arc.obj" : $(SOURCE) $(DEP_CPP_ARC_C) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\chatprot.cpp
DEP_CPP_CHATP=\
	".\avatar.h"\
	".\bbox.h"\
	".\binddoc.h"\
	".\bodycam.h"\
	".\chat.h"\
	".\chatDoc.h"\
	".\chatprot.h"\
	".\dib.h"\
	".\histent.h"\
	".\MainFrm.h"\
	".\PageView.h"\
	".\pe.h"\
	".\script.h"\
	".\StdAfx.h"\
	".\ui.h"\
	".\userinfo.h"\
	

"$(INTDIR)\chatprot.obj" : $(SOURCE) $(DEP_CPP_CHATP) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\avatardl.cpp
DEP_CPP_AVATAR=\
	".\avatar.h"\
	".\avatardl.h"\
	".\bbox.h"\
	".\bodycam.h"\
	".\chat.h"\
	".\chatprot.h"\
	".\dib.h"\
	".\histent.h"\
	".\memblst.h"\
	".\pe.h"\
	".\StdAfx.h"\
	".\ui.h"\
	".\userinfo.h"\
	

"$(INTDIR)\avatardl.obj" : $(SOURCE) $(DEP_CPP_AVATAR) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\setupdlg.cpp
DEP_CPP_SETUP=\
	".\avatar.h"\
	".\backdrop.h"\
	".\balloon.h"\
	".\bbox.h"\
	".\chat.h"\
	".\chatprot.h"\
	".\dib.h"\
	".\helpids.h"\
	".\histent.h"\
	".\panel.h"\
	".\pe.h"\
	".\setupdlg.h"\
	".\spline.h"\
	".\StdAfx.h"\
	".\traj.h"\
	".\ui.h"\
	".\userinfo.h"\
	

"$(INTDIR)\setupdlg.obj" : $(SOURCE) $(DEP_CPP_SETUP) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\avatario.cpp
DEP_CPP_AVATARI=\
	".\avatar.h"\
	".\avatario.h"\
	".\bbox.h"\
	".\chat.h"\
	".\dib.h"\
	".\pe.h"\
	".\StdAfx.h"\
	".\vector2d.h"\
	

"$(INTDIR)\avatario.obj" : $(SOURCE) $(DEP_CPP_AVATARI) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\semantic.cpp
DEP_CPP_SEMAN=\
	".\avatar.h"\
	".\backdrop.h"\
	".\balloon.h"\
	".\bbox.h"\
	".\chat.h"\
	".\chatprot.h"\
	".\common.h"\
	".\dib.h"\
	".\panel.h"\
	".\pe.h"\
	".\spline.h"\
	".\StdAfx.h"\
	".\traj.h"\
	

"$(INTDIR)\semantic.obj" : $(SOURCE) $(DEP_CPP_SEMAN) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\splinutl.cpp
DEP_CPP_SPLINU=\
	".\chat.h"\
	".\spline.h"\
	".\StdAfx.h"\
	".\traj.h"\
	".\vector2d.h"\
	

"$(INTDIR)\splinutl.obj" : $(SOURCE) $(DEP_CPP_SPLINU) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\memblst.cpp
DEP_CPP_MEMBL=\
	".\avatar.h"\
	".\bbox.h"\
	".\binddoc.h"\
	".\chat.h"\
	".\chatDoc.h"\
	".\chatprot.h"\
	".\dib.h"\
	".\memblst.h"\
	".\pe.h"\
	".\saywnd.h"\
	".\StdAfx.h"\
	".\textview.h"\
	".\ui.h"\
	".\userinfo.h"\
	

"$(INTDIR)\memblst.obj" : $(SOURCE) $(DEP_CPP_MEMBL) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\fonts.cpp
DEP_CPP_FONTS=\
	".\avatar.h"\
	".\backdrop.h"\
	".\balloon.h"\
	".\bbox.h"\
	".\chat.h"\
	".\common.h"\
	".\dib.h"\
	".\panel.h"\
	".\pe.h"\
	".\spline.h"\
	".\StdAfx.h"\
	".\traj.h"\
	

"$(INTDIR)\fonts.obj" : $(SOURCE) $(DEP_CPP_FONTS) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\profdlg.cpp
DEP_CPP_PROFD=\
	".\chat.h"\
	".\profdlg.h"\
	".\StdAfx.h"\
	

"$(INTDIR)\profdlg.obj" : $(SOURCE) $(DEP_CPP_PROFD) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\histent.cpp
DEP_CPP_HISTE=\
	".\avatar.h"\
	".\bbox.h"\
	".\binddoc.h"\
	".\chat.h"\
	".\chatDoc.h"\
	".\chatprot.h"\
	".\dib.h"\
	".\histent.h"\
	".\memblst.h"\
	".\PageView.h"\
	".\pe.h"\
	".\script.h"\
	".\StdAfx.h"\
	".\textview.h"\
	".\ui.h"\
	".\userinfo.h"\
	

"$(INTDIR)\histent.obj" : $(SOURCE) $(DEP_CPP_HISTE) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\spltchat.cpp
DEP_CPP_SPLTC=\
	".\saywnd.h"\
	".\spltchat.h"\
	".\StdAfx.h"\
	".\ui.h"\
	

"$(INTDIR)\spltchat.obj" : $(SOURCE) $(DEP_CPP_SPLTC) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\textview.cpp
DEP_CPP_TEXTV=\
	".\binddoc.h"\
	".\chat.h"\
	".\chatDoc.h"\
	".\chatprot.h"\
	".\memblst.h"\
	".\saywnd.h"\
	".\StdAfx.h"\
	".\textview.h"\
	".\ui.h"\
	".\url.h"\
	".\userinfo.h"\
	

"$(INTDIR)\textview.obj" : $(SOURCE) $(DEP_CPP_TEXTV) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\roomlist.cpp
DEP_CPP_ROOML=\
	".\chat.h"\
	".\chatprot.h"\
	".\roomlist.h"\
	".\setupdlg.h"\
	".\StdAfx.h"\
	

"$(INTDIR)\roomlist.obj" : $(SOURCE) $(DEP_CPP_ROOML) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\proppage.cpp
DEP_CPP_PROPP=\
	".\avatar.h"\
	".\backdrop.h"\
	".\balloon.h"\
	".\bbox.h"\
	".\binddoc.h"\
	".\bodycam.h"\
	".\chat.h"\
	".\chatDoc.h"\
	".\chatprot.h"\
	".\dib.h"\
	".\histent.h"\
	".\memblst.h"\
	".\panel.h"\
	".\pe.h"\
	".\proppage.h"\
	".\setupdlg.h"\
	".\spline.h"\
	".\StdAfx.h"\
	".\traj.h"\
	".\ui.h"\
	".\userinfo.h"\
	

"$(INTDIR)\proppage.obj" : $(SOURCE) $(DEP_CPP_PROPP) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\admindlg.cpp
DEP_CPP_ADMIN=\
	".\admindlg.h"\
	".\chat.h"\
	".\StdAfx.h"\
	

"$(INTDIR)\admindlg.obj" : $(SOURCE) $(DEP_CPP_ADMIN) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
################################################################################
# Begin Source File

SOURCE=.\url.cpp
DEP_CPP_URL_C=\
	".\StdAfx.h"\
	".\ui.h"\
	".\url.h"\
	

"$(INTDIR)\url.obj" : $(SOURCE) $(DEP_CPP_URL_C) "$(INTDIR)"\
 "$(INTDIR)\chat.pch"


# End Source File
# End Target
# End Project
################################################################################
