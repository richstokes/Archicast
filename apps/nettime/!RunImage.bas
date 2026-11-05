REM NetTime: sets the clock from a network time server (NTP)
REM One request to port 123, then OS_Word 15. The clock chip model hands that to the
REM console's own clock as well. The DreamNet SWIs are described in docs/ARCHITECTURE.md
DIM b% 1024, cfg% 64, net% 80, pkt% 64, ibuf% 40, srv% 60, off% 12, msg% 80, msg2% 80
DIM rv% 8, ov% 20, name% 60, tb% 32
swi%=&CDC00
quit%=FALSE: state%=0: fd%=-1: tries%=0: sentat%=0: started%=0: ntpip%=0: ntpport%=123
SYS swi%+38,cfg%
$srv%=FNstr(cfg%+8)
$off%=FNoffset(cfg%!0)
SYS "Wimp_Initialise",200,&4B534154,"NetTime" TO ,task%
PROCwindow
PROCopen
PROCready
REPEAT
  SYS "OS_ReadMonotonicTime" TO now%
  IF state% THEN wait%=10 ELSE wait%=100
  SYS "Wimp_PollIdle",0,b%,now%+wait% TO r%
  CASE r% OF
    WHEN 0: PROCtick
    WHEN 1: PROCredraw
    WHEN 2: SYS "Wimp_OpenWindow",,b%
    WHEN 3: quit%=TRUE
    WHEN 6: PROCclick(b%!16)
    WHEN 8: PROCkey(b%!24)
    WHEN 17,18: IF b%!16=0 THEN quit%=TRUE
  ENDCASE
UNTIL quit%
IF fd%>=0 THEN SYS swi%+16,fd%
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

DEF PROCwindow
LOCAL f%
b%!0=200: b%!4=380: b%!8=960: b%!12=696
b%!16=0: b%!20=0: b%!24=-1: b%!28=&87000002
b%?32=7: b%?33=2: b%?34=7: b%?35=1: b%?36=3: b%?37=1: b%?38=12: b%?39=0
b%!40=0: b%!44=-316: b%!48=760: b%!52=0
b%!56=&2700003D: b%!60=0: b%!64=1: b%!68=0
$(b%+72)="Net Time"
b%!84=0
p%=88
$rv%="R7": $ov%="Soptoff,opton"
REM 1 the server, 3 the offset, 4 summer time, 5 and 6 buttons, 7 and 8 status
PROCicon(16,-68,196,-24,&17000111,FNtext("Server"),-1,8)
PROCicon(200,-68,744,-24,&0700F13D,srv%,rv%,56)
PROCicon(16,-124,196,-80,&17000111,FNtext("UTC offset"),-1,12)
PROCicon(200,-124,340,-80,&0700F13D,off%,rv%,10)
f%=&17000113 OR (11<<12)
IF cfg%!4 THEN f%=f% OR (1<<21)
PROCicon(370,-124,744,-80,f%,FNtext("Summer time"),ov%,12)
PROCicon(16,-188,256,-140,&1700313D,FNtext("Set clock"),-1,10)
PROCicon(268,-188,448,-140,&1700313D,FNtext("Save"),-1,5)
PROCicon(16,-252,744,-208,&17000131,msg%,-1,80)
PROCicon(16,-300,744,-256,&17000131,msg2%,-1,80)
b%!84=9
SYS "Wimp_CreateWindow",,b% TO win%
ENDPROC

DEF PROCopen
b%!0=win%
SYS "Wimp_GetWindowState",,b%
b%!28=-1
SYS "Wimp_OpenWindow",,b%
SYS "Wimp_SetCaretPosition",win%,1,0,0,-1,LEN($srv%)
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

DEF PROCmessage(a$)
PROClines(a$,"")
ENDPROC

DEF PROClines(a$,b$)
$msg%=LEFT$(a$,78): $msg2%=LEFT$(b$,78)
!ibuf%=win%: ibuf%!4=7: ibuf%!8=0: ibuf%!12=0
SYS "Wimp_SetIconState",,ibuf%
ibuf%!4=8
SYS "Wimp_SetIconState",,ibuf%
ENDPROC

DEF PROCready
SYS swi%+0,net%
IF !net%=2 THEN PROCmessage("Set clock asks the server for the time") ELSE PROCmessage(FNstr(net%+48))
ENDPROC

DEF PROCkey(k%)
CASE k% OF
  WHEN 13: PROCset
  WHEN &18A,&18E,&18F: PROCnextfield
  OTHERWISE: SYS "Wimp_ProcessKey",k%
ENDCASE
ENDPROC

REM Tab and the arrows move between the two fields
DEF PROCnextfield
LOCAL i%
SYS "Wimp_GetCaretPosition",,ibuf%
IF ibuf%!4=1 THEN i%=3 ELSE i%=1
IF i%=1 THEN SYS "Wimp_SetCaretPosition",win%,1,0,0,-1,LEN($srv%) ELSE SYS "Wimp_SetCaretPosition",win%,3,0,0,-1,LEN($off%)
ENDPROC

DEF PROCclick(i%)
CASE i% OF
  WHEN 5: PROCset
  WHEN 6: PROCsave
ENDCASE
ENDPROC

REM Minutes from UTC, from text like -8, +5:30 or 0. -9999 if it makes no sense.
DEF FNparse(a$)
LOCAL s%,h%,m%,c%
s%=1
IF LEFT$(a$,1)="+" THEN a$=MID$(a$,2)
IF LEFT$(a$,1)="-" THEN s%=-1: a$=MID$(a$,2)
IF a$="" THEN =-9999
c%=INSTR(a$,":")
IF c% THEN h%=VAL(LEFT$(a$,c%-1)): m%=VAL(MID$(a$,c%+1)) ELSE h%=VAL(a$): m%=0
IF h%<0 OR h%>14 OR m%<0 OR m%>59 THEN =-9999
=s%*(h%*60+m%)

DEF FNoffset(m%)
LOCAL a$
IF m%<0 THEN a$="-": m%=-m% ELSE a$="+"
=a$+STR$(m% DIV 60)+":"+RIGHT$("0"+STR$(m% MOD 60),2)

DEF FNtwo(n%)
=RIGHT$("0"+STR$(n%),2)

DEF FNcollect
LOCAL m%,a$
m%=FNparse(FNstr(off%))
IF m%=-9999 THEN
  PROCmessage("UTC offset is in hours, like -8 or +5:30")
  =FALSE
ENDIF
cfg%!0=m%
IF FNselected(4) THEN cfg%!4=1 ELSE cfg%!4=0
a$=FNstr(srv%)
$(cfg%+8)=a$
cfg%?(8+LEN(a$))=0
=TRUE

DEF PROCsave
LOCAL r%
IF NOT FNcollect THEN ENDPROC
SYS swi%+39,cfg% TO r%
IF r%=0 THEN PROCmessage("Saved to the VMU") ELSE PROCmessage("No VMU with a free block")
ENDPROC

DEF PROCset
LOCAL a$,c%
IF state% THEN ENDPROC
IF NOT FNcollect THEN ENDPROC
SYS swi%+0,net%
IF !net%<>2 THEN PROCmessage("The network isn't up yet: "+FNstr(net%+48)): ENDPROC
a$=FNstr(srv%): ntpport%=123
c%=INSTR(a$,":")
IF c% THEN ntpport%=VAL(MID$(a$,c%+1)): a$=LEFT$(a$,c%-1)
IF a$="" THEN PROCmessage("Fill in a time server, such as pool.ntp.org"): ENDPROC
$name%=a$
SYS swi%+5,name%
SYS "OS_ReadMonotonicTime" TO started%
state%=1: tries%=0
PROCmessage("Looking up "+a$)
ENDPROC

DEF PROCtick
LOCAL st%,ip%,n%,t%,from%,port%
SYS "OS_ReadMonotonicTime" TO t%
CASE state% OF
  WHEN 1
    SYS swi%+6 TO st%,ip%
    IF st%=1 THEN ntpip%=ip%: PROCsend
    IF st%<0 OR (st%=0 AND t%-started%>1000) THEN state%=0: PROCmessage("Could not find "+FNstr(name%))
  WHEN 2
    REM Only the server's answer: a late DNS reply can arrive on the same local port
    SYS swi%+15,fd%,pkt%,64 TO n%,,,from%,port%
    IF n%>=48 AND from%=ntpip% AND port%=ntpport% THEN
      PROCgot(t%)
    ELSE
      IF t%-sentat%>200 THEN
        IF tries%<3 THEN PROCsend ELSE state%=0: PROCmessage("No answer from "+FNstr(name%))
      ENDIF
    ENDIF
ENDCASE
ENDPROC

REM A client request: version 3, mode 3, everything else zero
DEF PROCsend
LOCAL i%,r%
IF fd%<0 THEN SYS swi%+9,1 TO fd%
IF fd%<0 THEN state%=0: PROCmessage("Could not open a socket ("+STR$(fd%)+")"): ENDPROC
FOR i%=0 TO 47: pkt%?i%=0: NEXT
?pkt%=&1B
SYS swi%+14,fd%,pkt%,48,ntpip%,ntpport% TO r%
IF r%<0 THEN state%=0: PROCmessage("Could not send ("+STR$(r%)+")"): ENDPROC
SYS "OS_ReadMonotonicTime" TO sentat%
tries%+=1: state%=2
PROCmessage("Asking "+FNstr(name%))
ENDPROC

REM The server's transmit time, seconds from 1900 in bytes 40-43 and a fraction in
REM 44-47, plus half the round trip. Era 1 starts in 2036, when the count wraps.
DEF PROCgot(t%)
LOCAL n,secs,old,cs%
state%=0
IF (pkt%?0 AND 7)<>4 OR pkt%?1=0 THEN PROCmessage("The server would not give the time"): ENDPROC
n=((pkt%?40*256.0+pkt%?41)*256+pkt%?42)*256+pkt%?43
IF n<2208988800 THEN n+=4294967296
cs%=pkt%?44*100 DIV 256+(t%-sentat%) DIV 2
SYS "OS_ReadMonotonicTime" TO now%
cs%+=now%-t%
secs=n-2208988800+cfg%!0*60+cfg%!4*3600+(cs%+50) DIV 100
old=FNnow
PROCsettime(secs)
PROClines(FNdiff(secs-old),"Now "+FNnowtext)
ENDPROC

REM RISC OS's time as seconds from 1970. OS_Word 14 reason 3 gives centiseconds from
REM 1900 in five bytes, too many for a real to hold exactly, so 1970 comes off first:
REM it's 13166 * 2^24 + 10054144 cs, and 2^24 is 167772 * 100 + 16.
DEF FNnow
LOCAL h%,l%
?tb%=3: SYS "OS_Word",14,tb%
h%=tb%?4*256+tb%?3-13166
l%=(tb%?2*256+tb%?1)*256+tb%?0-10054144
IF l%<0 THEN l%+=16777216: h%-=1
=h%*167772.0+(h%*16+l%) DIV 100

DEF FNnowtext
?tb%=0: SYS "OS_Word",14,tb%
=FNstr(tb%)

REM Seconds from 1970 to the string OS_Word 15 takes, by Howard Hinnant's algorithm
DEF PROCsettime(s)
LOCAL d%,r%,z%,era%,doe%,yoe%,doy%,mp%,dd%,m%,y%,a$
d%=INT(s/86400): r%=s-d%*86400.0
z%=d%+719468: era%=z% DIV 146097: doe%=z%-era%*146097
yoe%=(doe%-doe% DIV 1460+doe% DIV 36524-doe% DIV 146096) DIV 365
doy%=doe%-(365*yoe%+yoe% DIV 4-yoe% DIV 100)
mp%=(5*doy%+2) DIV 153
dd%=doy%-(153*mp%+2) DIV 5+1
IF mp%<10 THEN m%=mp%+3 ELSE m%=mp%-9
y%=yoe%+era%*400
IF m%<=2 THEN y%+=1
a$=MID$("SunMonTueWedThuFriSat",((d%+4) MOD 7)*3+1,3)+","+FNtwo(dd%)+" "
a$+=MID$("JanFebMarAprMayJunJulAugSepOctNovDec",m%*3-2,3)+" "+STR$(y%)+"."
a$+=FNtwo(r% DIV 3600)+":"+FNtwo(r% DIV 60 MOD 60)+":"+FNtwo(r% MOD 60)
?tb%=24: $(tb%+1)=a$
SYS "OS_Word",15,tb%
ENDPROC

DEF FNdiff(d)
LOCAL a$,days%,s%
IF ABS(d)<1 THEN ="The clock was already right"
IF d>0 THEN a$=" slow" ELSE a$=" fast"
d=ABS(d): days%=INT(d/86400): s%=d-days%*86400.0
a$=STR$(s% DIV 3600)+":"+FNtwo(s% DIV 60 MOD 60)+":"+FNtwo(s% MOD 60)+a$
IF days%=1 THEN a$="1 day "+a$
IF days%>1 THEN a$=STR$(days%)+" days "+a$
="Was "+a$
