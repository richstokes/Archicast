REM NetPrint: prints to a network printer through the ROM's own printer drivers
REM PDriverDP renders the pages, and the dumper in the DreamApps module hands them
REM to src/print.c. All this app does is tell PDriver about it, the job of !Printers
REM on a machine with its printer definitions on disc, so it has to run once.
DIM b% 1024, cfg% 64, stat% 64, ibuf% 40, host% 60, msg% 80, rv% 8, radio% 32
DIM dump% 256, drv% 256, name% 32, rect% 16, rm% 16, spot% 8, got% 16
swi%=&CDC00
DUMPER=40: DPDRIVER=7: COLS=90
quit%=FALSE: held%=0: job%=0: in%=0: was%=-1: asked%=0: from%=0
SCRAP$="<Wimp$Scrap>"
SYS "Wimp_Initialise",200,&4B534154,"NetPrint" TO ,task%
SYS swi%+26,cfg%
$host%=FNstr(cfg%+4)
PROCwindow
PROCiconbar("!netprint")
PROCdriver
PROCopen
PROCshow
ON ERROR PROCfailed: REPEAT: PROCpoll: UNTIL quit%: SYS "Wimp_CloseDown",task%,&4B534154: END
REPEAT: PROCpoll: UNTIL quit%
SYS "Wimp_CloseDown",task%,&4B534154
END

DEF PROCpoll
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
  WHEN 17,18: PROCwimpmsg
  WHEN 19: PROCbounced
ENDCASE
ENDPROC

REM The printer manager's half of printing from an application, as !Printers does it.
REM Print asks for a manager with PrintSave. PrintFile offers the job back, and an
REM application that replies WillPrint prints straight through PDriver to printer:.
REM If that bounces, PrintTypeOdd asks if it knows its own type: !Draw says PrintTypeKnown
REM and prints the same way. If that bounces too, !Edit for one, it's a file transfer.
REM We ask for the file like a drop on the icon bar, and print the text ourselves.
DEF PROCwimpmsg
CASE b%!16 OF
  WHEN 0: quit%=TRUE
  WHEN 1: IF b%!20=-2 THEN from%=b%!4: b%!12=b%!8: PROCwantfile
  WHEN 3: IF b%!20=-2 OR b%!12=asked% THEN PROCfile
  WHEN &80142: from%=b%!4: b%!12=b%!8: b%!16=&80140: SYS "Wimp_SendMessage",18,b%,0
  WHEN &80143: IF b%!4<>task% THEN quit%=TRUE
ENDCASE
ENDPROC

DEF PROCbounced
CASE b%!16 OF
  WHEN &80140: b%!0=20: b%!16=&80145: SYS "Wimp_SendMessage",18,b%,from%
  WHEN &80145: PROCwantfile
ENDCASE
ENDPROC

DEF PROCwantfile
b%!0=(48+LEN(SCRAP$)+4) AND NOT 3
b%!16=2: b%!36=-1: b%!40=&FFF
$(b%+44)=SCRAP$
b%?(44+LEN(SCRAP$))=0
SYS "Wimp_SendMessage",17,b%,from%
asked%=b%!8
ENDPROC

DEF PROCfile
LOCAL a$,type%
a$=FNstr(b%+44): type%=b%!40
b%!12=b%!8: b%!16=4
SYS "Wimp_SendMessage",17,b%,b%!4
IF type%<>&FFF THEN PROCmessage("Only text can be printed like that") ELSE PROCtext(a$)
IF a$=SCRAP$ THEN SYS "XOS_File",6,a$
ENDPROC

DEF PROCfailed
IF job% THEN
  SYS "XPDriver_AbortJob",job%
  SYS "XOS_Find",0,job%
  job%=0
ENDIF
IF in% THEN CLOSE#in%: in%=0
PROCmessage(REPORT$)
held%=30
ENDPROC

REM Letter and A4 in millipoints. The margins are what the printer reports, 4.32mm.
DEF PROCdriver
LOCAL xs%,ys%,m%,features%
IF cfg%!0=1 THEN xs%=595276: ys%=841890 ELSE xs%=612000: ys%=792000
m%=12250
SYS "XPDriver_SelectDriver",DPDRIVER
drv%?0=16: drv%?1=0: drv%?2=0: drv%?3=0
drv%?4=0: drv%?5=0: drv%?6=1: drv%?7=0
SYS "PDriver_SetDriver",,DUMPER,0,dump%,drv%,0
SYS "PDriver_Info" TO ,,,features%
$name%="Network printer"
SYS "PDriver_SetInfo",,300,300,features%,name%,75,75,0
SYS "PDriver_SetPageSize",,xs%,ys%,m%,m%,xs%-m%,ys%-m%
ENDPROC

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

DEF PROCradio(x%,y%,a$,on%)
LOCAL f%
f%=&17000113 OR (11<<12) OR (1<<16)
IF on% THEN f%=f% OR (1<<21)
PROCicon(x%,y%,x%+180,y%+44,f%,FNtext(a$),radio%,LEN(a$)+1)
ENDPROC

DEF PROCwindow
b%!0=240: b%!4=340: b%!8=860: b%!12=620
b%!16=0: b%!20=0: b%!24=-1: b%!28=&87000002
b%?32=7: b%?33=2: b%?34=7: b%?35=1: b%?36=3: b%?37=1: b%?38=12: b%?39=0
b%!40=0: b%!44=-280: b%!48=620: b%!52=0
b%!56=&2700003D: b%!60=0: b%!64=1: b%!68=0
$(b%+72)="Net Print"
b%!84=0
p%=88
$rv%="R7": $radio%="Sradiooff,radioon"
REM 0 and 1 the paper, 2 the address label, 3 the address, 4 to 6 buttons, 7 status
PROCradio(150,-120,"Letter",cfg%!0=0)
PROCradio(340,-120,"A4",cfg%!0=1)
PROCicon(16,-68,146,-24,&17000111,FNtext("Printer"),-1,8)
PROCicon(150,-68,600,-24,&0700F13D,host%,rv%,60)
PROCicon(16,-196,196,-148,&1700313D,FNtext("Apply"),-1,6)
PROCicon(208,-196,388,-148,&1700313D,FNtext("Save"),-1,5)
PROCicon(400,-196,600,-148,&1700313D,FNtext("Test page"),-1,10)
PROCicon(16,-260,600,-216,&17000131,msg%,-1,80)
b%!84=8
SYS "Wimp_CreateWindow",,b% TO win%
ENDPROC

DEF PROCiconbar(sprite$)
DIM menu% 64
b%!0=-1: b%!4=0: b%!8=0: b%!12=68: b%!16=68: b%!20=&3002
$(b%+24)=sprite$
SYS "Wimp_CreateIcon",,b%
$menu%="NetPrint": menu%?12=7: menu%?13=2: menu%?14=7: menu%?15=0
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

DEF FNselected(i%)
!ibuf%=win%: ibuf%!4=i%
SYS "Wimp_GetIconState",,ibuf%
=(ibuf%!24 AND (1<<21))<>0

DEF PROCcollect
LOCAL a$
IF FNselected(1) THEN cfg%!0=1 ELSE cfg%!0=0
a$=FNstr(host%)
$(cfg%+4)=a$
cfg%?(4+LEN(a$))=0
SYS swi%+27,cfg%
PROCdriver
ENDPROC

DEF PROCclick(i%)
LOCAL r%
CASE i% OF
  WHEN 4: PROCcollect: PROCmessage("Applied")
  WHEN 5
    PROCcollect
    SYS swi%+28,cfg% TO r%
    IF r%=0 THEN PROCmessage("Saved to the VMU") ELSE PROCmessage("No VMU with a free block")
  WHEN 6: PROCcollect: PROCtest
ENDCASE
ENDPROC

DEF PROCmessage(a$)
$msg%=a$
!ibuf%=win%: ibuf%!4=7: ibuf%!8=0: ibuf%!12=0
SYS "Wimp_SetIconState",,ibuf%
held%=4
ENDPROC

DEF PROCshow
IF held%>0 THEN held%-=1: ENDPROC
SYS swi%+29,stat%
IF !stat%=0 AND was%=-1 THEN
  IF FNstr(host%)="" THEN $msg%="Set the printer's address" ELSE $msg%="Ready"
ELSE
  $msg%=FNstr(stat%+12)
ENDIF
!ibuf%=win%: ibuf%!4=7: ibuf%!8=0: ibuf%!12=0
SYS "Wimp_SetIconState",,ibuf%
ENDPROC

REM A page through PDriver, as any application would do it. Each rectangle is one
REM strip, so the drawing runs several times.
DEF PROCtest
LOCAL more%,id%
PROCmessage("Printing a test page")
SYS "OS_Find",&8F,"printer:" TO job%
SYS "PDriver_SelectJob",job%,"NetPrint test"
!rect%=0: rect%!4=0: rect%!8=1400: rect%!12=1800
!rm%=&10000: rm%!4=0: rm%!8=0: rm%!12=&10000
!spot%=36000: spot%!4=36000
SYS "PDriver_GiveRectangle",1,rect%,rm%,spot%,&FFFFFF00
SYS "PDriver_DrawPage",1,got%,0,0 TO more%,,id%
WHILE more%
  PROCpage
  SYS "PDriver_GetRectangle",,got% TO more%,,id%
ENDWHILE
SYS "PDriver_EndJob",job%
SYS "OS_Find",0,job%
job%=0
ENDPROC

REM Text in Corpus, one rectangle per page. Each page redraws once per strip, so it
REM goes back to where it started, and what's left over is the next page's.
DEF PROCtext(a$)
LOCAL more%,id%,page%,start%,stop%,size%
PROCmessage("Printing")
in%=OPENIN(a$)
SYS "OS_Args",2,in% TO ,,size%
SYS "OS_Find",&8F,"printer:" TO job%
SYS "PDriver_SelectJob",job%,"NetPrint text"
PROCarea
!rect%=0: rect%!4=0: rect%!8=pw%: rect%!12=ph%
!rm%=&10000: rm%!4=0: rm%!8=0: rm%!12=&10000
!spot%=36000: spot%!4=36000
page%=1: start%=0
REPEAT
  SYS "PDriver_GiveRectangle",1,rect%,rm%,spot%,&FFFFFF00
  SYS "PDriver_DrawPage",1,got%,page%,0 TO more%,,id%
  stop%=start%
  WHILE more%
    stop%=FNtextpage(start%)
    SYS "PDriver_GetRectangle",,got% TO more%,,id%
  ENDWHILE
  IF stop%=start% THEN stop%=FNtextpage(start%)
  start%=stop%: page%+=1
UNTIL start%>=size%
SYS "PDriver_EndJob",job%
SYS "OS_Find",0,job%
job%=0
CLOSE#in%: in%=0
ENDPROC

REM The sheet less half inch margins, in OS units
DEF PROCarea
IF cfg%!0=1 THEN pw%=1308: ph%=1924 ELSE pw%=1350: ph%=1800
ENDPROC

DEF FNtextpage(start%)
LOCAL f%,y%,a$,n%,i%
SYS "Font_FindFont",,"Corpus.Medium",10*16,10*16,0,0 TO f%
SYS "ColourTrans_SetFontColours",f%,&FFFFFF00,0,14
SYS "OS_Args",1,in%,start%
y%=ph%-30
WHILE y%>=0 AND NOT EOF#in%
  SYS "OS_Args",0,in% TO ,,n%
  a$=GET$#in%
  IF LEN(a$)>COLS THEN a$=LEFT$(a$,COLS): SYS "OS_Args",1,in%,n%+COLS
  FOR i%=1 TO LEN(a$)
    IF ASC(MID$(a$,i%,1))<32 THEN MID$(a$,i%,1)=" "
  NEXT
  IF a$<>"" THEN SYS "Font_Paint",f%,a$,&110,0,y%
  y%-=30
ENDWHILE
SYS "Font_LoseFont",f%
SYS "OS_Args",0,in% TO ,,n%
=n%

DEF PROCpage
LOCAL f%,i%
SYS "Font_FindFont",,"Trinity.Medium",48*16,48*16,0,0 TO f%
SYS "ColourTrans_SetFontColours",f%,&FFFFFF00,0,14
SYS "Font_Paint",f%,"RISC OS on a Dreamcast",&110,60,1680
SYS "Font_LoseFont",f%
SYS "Font_FindFont",,"Homerton.Medium",14*16,14*16,0,0 TO f%
SYS "ColourTrans_SetFontColours",f%,&FFFFFF00,0,14
SYS "Font_Paint",f%,"Printed through PDriverDP, URF and IPP. The quick brown fox jumps.",&110,60,1600
SYS "Font_LoseFont",f%
SYS "ColourTrans_SetGCOL",0,,0
RECTANGLE 60,1000,1280,500
MOVE 60,1000: DRAW 1340,1500
MOVE 60,1500: DRAW 1340,1000
CIRCLE 700,1250,200
FOR i%=0 TO 8
  SYS "ColourTrans_SetGCOL",(i%*31)<<8 OR (i%*31)<<16 OR (i%*31)<<24,,0
  RECTANGLE FILL 60+i%*140,700,130,200
NEXT
ENDPROC
