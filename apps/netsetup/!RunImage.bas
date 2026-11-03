REM NetSetup: choose how the Dreamcast reaches the network
REM Auto tries the Broadband then LAN Adapter with DHCP, which needs no setup.
REM The W5500 is never probed unless it is picked here, and the modem has to be
REM picked too as it ties up a phone line. Save keeps the choice on a VMU.
REM TCP/IP is native and runs without this, the icon is just for status.
DIM b% 2048, cfg% 80, stat% 80, ibuf% 40
DIM ipt% 20, mask% 20, gwt% 20, dnst% 20, phone% 24, user% 16, pass% 16, msg% 80, addr% 80
DIM rv% 32, radio% 32
swi%=&CDC00
LINKS=5
quit%=FALSE: held%=0
SYS "Wimp_Initialise",200,&4B534154,"NetSetup" TO ,task%
SYS swi%+7,cfg%
PROCfields
PROCwindow
PROCiconbar("!netsetup")
PROCopen
PROCshow
REPEAT
  SYS "OS_ReadMonotonicTime" TO now%
  SYS "Wimp_PollIdle",0,b%,now%+50 TO r%
  CASE r% OF
    WHEN 0: PROCshow
    WHEN 1: PROCredraw
    WHEN 2: SYS "Wimp_OpenWindow",,b%
    WHEN 3: SYS "Wimp_CloseWindow",,b%
    WHEN 6: IF b%!12=-2 THEN PROCbarclick ELSE PROCclick(b%!16)
    WHEN 8: SYS "Wimp_ProcessKey",b%!24
    WHEN 9: quit%=TRUE
    WHEN 17,18: IF b%!16=0 THEN quit%=TRUE
  ENDCASE
UNTIL quit%
SYS "Wimp_CloseDown",task%,&4B534154
END

DEF PROCicon(x0%,y0%,x1%,y1%,flags%,text%,valid%,len%)
b%!p%=x0%: b%!(p%+4)=y0%: b%!(p%+8)=x1%: b%!(p%+12)=y1%
b%!(p%+16)=flags%: b%!(p%+20)=text%: b%!(p%+24)=valid%: b%!(p%+28)=len%
p%+=32
ENDPROC

DEF FNtext(a$)
LOCAL t%
DIM t% LEN(a$)+1
$t%=a$
=t%

DEF PROCradio(i%,y%,a$,on%,esg%)
LOCAL f%
f%=&17000113 OR (11<<12) OR (esg%<<16)
IF on% THEN f%=f% OR (1<<21)
PROCicon(16,y%,352,y%+44,f%,FNtext(a$),radio%,LEN(a$)+1)
ENDPROC

DEF PROCfield(y%,a$,t%,len%)
PROCicon(356,y%,500,y%+44,&17000111,FNtext(a$),-1,LEN(a$)+1)
PROCicon(504,y%,800,y%+44,&0700F13D,t%,rv%,len%)
ENDPROC

DEF PROCwindow
LOCAL i%,names$,a$
b%!0=160: b%!4=160: b%!8=980: b%!12=760
b%!16=0: b%!20=0: b%!24=-1: b%!28=&87000002
b%?32=7: b%?33=2: b%?34=7: b%?35=1: b%?36=3: b%?37=1: b%?38=12: b%?39=0
b%!40=0: b%!44=-600: b%!48=820: b%!52=0
b%!56=&2700003D: b%!60=0: b%!64=1: b%!68=0
$(b%+72)="Net Setup"
b%!84=0
p%=88
$rv%="R7": $radio%="Sradiooff,radioon"
REM icons 0-4 pick the link, 5-6 the addressing, then pairs of label and field
PROCradio(0,-60,"Auto",cfg%!0=0,1)
PROCradio(1,-108,"Broadband Adapter",cfg%!0=1,1)
PROCradio(2,-156,"LAN Adapter",cfg%!0=2,1)
PROCradio(3,-204,"W5500 (serial)",cfg%!0=3,1)
PROCradio(4,-252,"Modem / DreamPi",cfg%!0=4,1)
PROCradio(5,-324,"DHCP",cfg%!4<>0,2)
PROCradio(6,-372,"Fixed address",cfg%!4=0,2)
PROCfield(-60,"Address",ipt%,20)
PROCfield(-108,"Netmask",mask%,20)
PROCfield(-156,"Gateway",gwt%,20)
PROCfield(-204,"DNS",dnst%,20)
PROCfield(-252,"Phone",phone%,24)
PROCfield(-300,"User",user%,16)
PROCfield(-348,"Password",pass%,16)
i%=(p%-88) DIV 32
PROCicon(16,-444,196,-396,&1700313D,FNtext("Connect"),-1,8)
PROCicon(208,-444,388,-396,&1700313D,FNtext("Disconnect"),-1,11)
PROCicon(400,-444,580,-396,&1700313D,FNtext("Save"),-1,5)
btn%=i%
PROCicon(16,-500,800,-456,&17000131,msg%,-1,80)
PROCicon(16,-552,800,-508,&17000131,addr%,-1,80)
stat1%=i%+3: stat2%=i%+4
b%!84=i%+5
SYS "Wimp_CreateWindow",,b% TO win%
ENDPROC


DEF PROCiconbar(sprite$)
DIM menu% 64
b%!0=-1: b%!4=0: b%!8=0: b%!12=68: b%!16=68: b%!20=&3002
$(b%+24)=sprite$
SYS "Wimp_CreateIcon",,b%
$menu%="NetSetup": menu%?12=7: menu%?13=2: menu%?14=7: menu%?15=0
menu%!16=112: menu%!20=44: menu%!24=0
menu%!28=&80: menu%!32=-1: menu%!36=&07000021
$(menu%+40)="Quit"
ENDPROC

DEF PROCbarclick
IF b%!8=2 THEN
  SYS "Wimp_CreateMenu",,menu%,b%!0-56,140
ELSE
  PROCopen
ENDIF
ENDPROC

DEF PROCopen
b%!0=win%
SYS "Wimp_GetWindowState",,b%
b%!28=-1
SYS "Wimp_OpenWindow",,b%
ENDPROC

DEF PROCredraw
LOCAL more%
SYS "Wimp_RedrawWindow",,b% TO more%
WHILE more%
  SYS "Wimp_GetRectangle",,b% TO more%
ENDWHILE
ENDPROC

DEF FNstr(a%)
LOCAL s$
WHILE ?a%>=32: s$+=CHR$(?a%): a%+=1: ENDWHILE
=s$

DEF FNdots(a%)
=STR$(?a%)+"."+STR$(a%?1)+"."+STR$(a%?2)+"."+STR$(a%?3)

DEF PROCparse(a$,d%)
LOCAL i%,n%
FOR i%=0 TO 3
  d%?i%=VAL(a$)
  n%=INSTR(a$,".")
  IF n%=0 THEN a$="0" ELSE a$=MID$(a$,n%+1)
NEXT
ENDPROC

DEF PROCfields
$ipt%=FNdots(cfg%+8): $mask%=FNdots(cfg%+12): $gwt%=FNdots(cfg%+16): $dnst%=FNdots(cfg%+20)
$phone%=FNstr(cfg%+24): $user%=FNstr(cfg%+48): $pass%=FNstr(cfg%+64)
ENDPROC

DEF PROCterm(a$,d%,len%)
$d%=LEFT$(a$,len%-1)
d%?LEN(LEFT$(a$,len%-1))=0
ENDPROC

DEF FNselected(i%)
!ibuf%=win%: ibuf%!4=i%
SYS "Wimp_GetIconState",,ibuf%
=(ibuf%!24 AND (1<<21))<>0

DEF PROCcollect
LOCAL i%
cfg%!0=0
FOR i%=1 TO LINKS-1
  IF FNselected(i%) THEN cfg%!0=i%
NEXT
IF FNselected(5) THEN cfg%!4=1 ELSE cfg%!4=0
PROCparse(FNstr(ipt%),cfg%+8): PROCparse(FNstr(mask%),cfg%+12)
PROCparse(FNstr(gwt%),cfg%+16): PROCparse(FNstr(dnst%),cfg%+20)
PROCterm(FNstr(phone%),cfg%+24,24)
PROCterm(FNstr(user%),cfg%+48,16)
PROCterm(FNstr(pass%),cfg%+64,16)
ENDPROC

DEF PROCclick(i%)
LOCAL r%
IF i%=btn% THEN PROCcollect: SYS swi%+1,cfg%
IF i%=btn%+1 THEN SYS swi%+2
IF i%=btn%+2 THEN
  PROCcollect
  SYS swi%+8,cfg% TO r%
  IF r%=0 THEN PROCmessage("Saved to the VMU") ELSE PROCmessage("No VMU with a free block")
  ENDPROC
ENDIF
IF i%>=btn% THEN PROCshow
ENDPROC

DEF PROCmessage(a$)
$msg%=a$
!ibuf%=win%: ibuf%!4=stat1%: ibuf%!8=0: ibuf%!12=0
SYS "Wimp_SetIconState",,ibuf%
held%=2
ENDPROC

DEF PROCshow
LOCAL a$
SYS swi%+0,stat%
IF held%>0 THEN
  held%-=1
ELSE
  $msg%=FNstr(stat%+48)
  !ibuf%=win%: ibuf%!4=stat1%: ibuf%!8=0: ibuf%!12=0
  SYS "Wimp_SetIconState",,ibuf%
ENDIF
IF !stat%=2 THEN
  a$=FNstr(stat%+32)+": "+FNdots(stat%+8)+"  gateway "+FNdots(stat%+16)
ELSE
  a$=""
ENDIF
$addr%=a$
!ibuf%=win%: ibuf%!4=stat2%: ibuf%!8=0: ibuf%!12=0
SYS "Wimp_SetIconState",,ibuf%
ENDPROC
