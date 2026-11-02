REM Find: searches the archive disc's index, then walks the other drives
REM The search itself is native, see DN_FIND in src/net.c
DIM b% 2048, q% 64, stat% 80, walkbuf% 1024
swi%=&CDC00
findop%=30: lineop%=31: driveop%=32
maxres%=200: rowh%=40: wide%=1216: namecol%=36: task%=0
DIM res%(maxres%-1), live$(maxres%-1), stack$(100)
buf%=0: bufend%=0: first%=0: count%=0: sel%=-1
sp%=0: walking%=FALSE: pend$="": pendat%=0: quit%=FALSE
$q%="": $stat%="Type a name and press Return"
ON ERROR PROCfail: END
SYS swi%+driveop% TO arc%
SYS "Wimp_Initialise",200,&4B534154,"Find" TO ,task%
PROCwindows
PROCopenwin(win%,-1)
SYS "Wimp_SetCaretPosition",win%,1,0,0,-1,0
REPEAT
  SYS "OS_ReadMonotonicTime" TO now%
  mask%=1
  IF walking% OR pend$<>"" THEN mask%=0
  SYS "Wimp_PollIdle",mask%,b%,now%+25 TO r%
  CASE r% OF
    WHEN 0: PROCtick
    WHEN 1: PROCredraw
    WHEN 2: SYS "Wimp_OpenWindow",,b%
    WHEN 3: IF !b%=win% THEN quit%=TRUE ELSE SYS "Wimp_CloseWindow",,b%
    WHEN 6: PROCclick
    WHEN 8: IF b%!24=13 THEN PROCsearch ELSE SYS "Wimp_ProcessKey",b%!24
    WHEN 17,18: IF b%!16=0 THEN quit%=TRUE
  ENDCASE
UNTIL quit%
SYS "Wimp_CloseDown",task%,&4B534154
END

DEF PROCfail
!b%=ERR
$(b%+4)=REPORT$+" at line "+STR$(ERL)+CHR$(0)
SYS "Wimp_ReportError",b%,1,"Find"
SYS "Wimp_CloseDown",task%,&4B534154
ENDPROC

DEF PROCicon(x0%,y0%,x1%,y1%,flags%,text%,valid%,len%)
b%!p%=x0%: b%!(p%+4)=y0%: b%!(p%+8)=x1%: b%!(p%+12)=y1%
b%!(p%+16)=flags%: b%!(p%+20)=text%: b%!(p%+24)=valid%: b%!(p%+28)=len%
p%+=32
ENDPROC

DEF PROCwinblock(x0%,y0%,x1%,y1%,flags%,w%,h%,btn%,title$)
b%!0=x0%: b%!4=y0%: b%!8=x1%: b%!12=y1%
b%!16=0: b%!20=0: b%!24=-1: b%!28=flags%
b%?32=7: b%?33=2: b%?34=7: b%?35=1: b%?36=3: b%?37=1: b%?38=12: b%?39=0
b%!40=0: b%!44=-h%: b%!48=w%: b%!52=0
b%!56=&2700003D: b%!60=btn%: b%!64=1: b%!68=0
$(b%+72)=title$
ENDPROC

DEF PROCwindows
LOCAL label%, go%, valid%
DIM label% 8, go% 8, valid% 8
$label%="Find": $go%="Search": $valid%="R7"
PROCwinblock(30,740,30+wide%,880,&87000002,wide%,132,0,"Find")
b%!84=4
p%=88
PROCicon(16,-60,96,-12,&17000111,label%,-1,8)
PROCicon(100,-60,1040,-12,&0700F135,q%,valid%,64)
PROCicon(1056,-60,1200,-12,&1700313D,go%,-1,8)
PROCicon(16,-120,1200,-68,&17000131,stat%,-1,80)
SYS "Wimp_CreateWindow",,b% TO win%
PROCwinblock(30,150,30+wide%,690,&FF000002,wide%,540,&A000,"Results")
b%!84=0
SYS "Wimp_CreateWindow",,b% TO list%
ENDPROC

DEF PROCopenwin(w%,behind%)
b%!0=w%
SYS "Wimp_GetWindowState",,b%
b%!28=behind%
SYS "Wimp_OpenWindow",,b%
ENDPROC

DEF PROCstatus(a$)
$stat%=LEFT$(a$,78)
!b%=win%: b%!4=3: b%!8=0: b%!12=0
SYS "Wimp_SetIconState",,b%
ENDPROC

DEF FNlower(a$)
LOCAL i%,c%
FOR i%=1 TO LEN(a$)
  c%=ASC(MID$(a$,i%,1))
  IF c%>=65 AND c%<=90 THEN MID$(a$,i%,1)=CHR$(c%+32)
NEXT
=a$

DEF FNstr(a%)
LOCAL s$
WHILE ?a%>=32 AND LEN(s$)<250: s$+=CHR$(?a%): a%+=1: ENDWHILE
=s$

DEF FNfield(a%,n%)
WHILE n%>0
  WHILE ?a%>=32: a%+=1: ENDWHILE
  IF ?a%<>9 THEN =""
  a%+=1: n%-=1
ENDWHILE
=FNstr(a%)

DEF PROCload
LOCAL t%,len%,name$
IF buf% OR arc%<0 THEN ENDPROC
name$=FNarc+"Index"
SYS "XOS_File",5,name$ TO t%,,,,len%
IF t%<>1 THEN ENDPROC
PROCstatus("Loading the index")
SYS "Hourglass_On"
DIM buf% len%+1
SYS "OS_File",255,name$,buf%,0
SYS "Hourglass_Off"
bufend%=buf%+len%
first%=buf%
ndirs%=VAL(FNstr(buf%))
SYS swi%+lineop%,buf%,bufend%,ndirs%+1 TO first%
ENDPROC

DEF FNarc
="adfs::"+STR$(arc%)+".$."

DEF PROCsearch
LOCAL a%,pat$
pat$=FNlower(FNstr(q%))
IF pat$="" THEN ENDPROC
$q%=pat$
PROCload
count%=0: sel%=-1: walking%=FALSE: sp%=0
needle$=pat$
a%=first%
IF buf% THEN
  REPEAT
    SYS swi%+findop%,a%,bufend%,q% TO a%
    IF a% THEN
      res%(count%)=a%: count%+=1
      WHILE ?a%>=32 OR ?a%=9: a%+=1: ENDWHILE
      a%+=1
    ENDIF
  UNTIL a%=0 OR count%=maxres%
ENDIF
PROCresize
SYS swi%+driveop% TO arc%
IF arc%<>4 THEN PROCpush("adfs::4.$")
PROCpush("adfs::0.$")
walking%=TRUE
PROCstatus(FNcount+", searching the other drives")
ENDPROC

DEF FNcount
IF count%=maxres% THEN ="First "+STR$(maxres%)+" results"
IF count%=1 THEN ="1 result"
=STR$(count%)+" results"

DEF PROCpush(a$)
IF sp%<=100 THEN stack$(sp%)=a$: sp%+=1
ENDPROC

DEF PROCtick
SYS "OS_ReadMonotonicTime" TO now%
IF pend$<>"" AND now%-pendat%>=0 THEN
  OSCLI "Filer_Run "+pend$
  pend$=""
ENDIF
IF NOT walking% THEN ENDPROC
IF sp%=0 OR count%>=maxres% THEN
  walking%=FALSE
  PROCstatus(FNcount)
  ENDPROC
ENDIF
sp%-=1
PROCwalk(stack$(sp%))
ENDPROC

DEF PROCwalk(dir$)
LOCAL n%,off%,f%,e%,t%,name$
REPEAT
  SYS "XOS_GBPB",10,dir$,walkbuf%,20,off%,1024,0 TO ,,,n%,off%;f%
  IF f% AND 1 THEN ENDPROC
  e%=walkbuf%
  WHILE n%>0 AND count%<maxres%
    t%=e%!16
    name$=FNzero(e%+20)
    IF INSTR(FNlower(name$),needle$) THEN
      live$(count%)=dir$+"."+name$
      res%(count%)=0: count%+=1
      PROCresize
    ENDIF
    IF t%=2 THEN PROCpush(dir$+"."+name$)
    e%+=(20+LEN(name$)+4) AND NOT 3
    n%-=1
  ENDWHILE
UNTIL off%=-1
ENDPROC

DEF FNzero(a%)
LOCAL s$
WHILE ?a%: s$+=CHR$(?a%): a%+=1: ENDWHILE
=s$

DEF PROCresize
LOCAL h%
h%=count%*rowh%
IF h%<540 THEN h%=540
b%!0=0: b%!4=-h%: b%!8=wide%: b%!12=0
SYS "Wimp_SetExtent",list%,b%
PROCopenwin(list%,-1)
SYS "Wimp_ForceRedraw",list%,0,-h%,wide%,0
ENDPROC

DEF FNname(i%)
LOCAL a%,s$
a%=res%(i%)
IF a%=0 THEN
  s$=live$(i%)
  WHILE INSTR(s$,"."): s$=MID$(s$,INSTR(s$,".")+1): ENDWHILE
  =s$
ENDIF
s$=FNstr(a%+1)
IF INSTR(s$,CHR$(9)) THEN s$=LEFT$(s$,INSTR(s$,CHR$(9))-1)
=s$

DEF FNwhere(i%)
LOCAL a%,d%
a%=res%(i%)
IF a%=0 THEN =LEFT$(live$(i%),8)
IF ?a%<>ASC("F") THEN =FNfield(a%,1)
d%=VAL(FNfield(a%,1))
SYS swi%+lineop%,buf%,bufend%,d%+1 TO a%
=FNstr(a%)

DEF PROCredraw
LOCAL more%,ox%,oy%
SYS "Wimp_RedrawWindow",,b% TO more%
ox%=b%!4-b%!20
oy%=b%!16-b%!24
WHILE more%
  PROCdraw(ox%,oy%)
  SYS "Wimp_GetRectangle",,b% TO more%
ENDWHILE
ENDPROC

DEF PROCdraw(ox%,oy%)
LOCAL i%,top%,bot%,y%
IF !b%<>list% THEN ENDPROC
top%=(oy%-b%!40) DIV rowh%
bot%=(oy%-b%!32) DIV rowh%
IF top%<0 THEN top%=0
IF bot%>count%-1 THEN bot%=count%-1
VDU 5
FOR i%=top% TO bot%
  y%=oy%-i%*rowh%
  IF i%=sel% THEN
    SYS "Wimp_SetColour",7
    RECTANGLE FILL ox%,y%-rowh%,wide%,rowh%
    SYS "Wimp_SetColour",0
  ELSE
    SYS "Wimp_SetColour",7
  ENDIF
  MOVE ox%+12,y%-6
  PRINT LEFT$(FNname(i%),namecol%);
  MOVE ox%+12+(namecol%+1)*16,y%-6
  PRINT LEFT$(FNwhere(i%),36);
NEXT
VDU 4
ENDPROC

DEF PROCclick
LOCAL row%,my%,btn%
my%=b%!4: btn%=b%!8
IF !(b%+12)=win% THEN
  IF b%!16=2 THEN PROCsearch
  ENDPROC
ENDIF
IF b%!12<>list% THEN ENDPROC
!b%=list%
SYS "Wimp_GetWindowState",,b%
row%=(b%!16-b%!24-my%) DIV rowh%
IF row%<0 OR row%>=count% THEN ENDPROC
IF btn%=4 THEN PROCgo(row%): ENDPROC
IF btn%=1024 THEN PROCselect(row%)
ENDPROC

DEF PROCselect(row%)
LOCAL old%
old%=sel%: sel%=row%
IF old%>=0 THEN SYS "Wimp_ForceRedraw",list%,0,-(old%+1)*rowh%,wide%,-old%*rowh%
SYS "Wimp_ForceRedraw",list%,0,-(row%+1)*rowh%,wide%,-row%*rowh%
ENDPROC

DEF FNparent(a$)
LOCAL i%,j%
i%=INSTR(a$,".")
WHILE i%: j%=i%: i%=INSTR(a$,".",i%+1): ENDWHILE
=LEFT$(a$,j%-1)

DEF PROCgo(row%)
LOCAL a%,path$
a%=res%(row%)
IF a%=0 THEN
  path$=live$(row%)
  OSCLI "Filer_OpenDir "+FNparent(path$)
  pend$=path$
ELSE
  IF ?a%=ASC("D") THEN OSCLI "Filer_OpenDir "+FNarc+FNfield(a%,2): ENDPROC
  IF ?a%=ASC("R") THEN path$=FNfield(a%,2) ELSE path$=FNwhere(row%)+"."+FNname(row%)
  path$=FNarc+path$
  OSCLI "Filer_OpenDir "+FNparent(path$)
  pend$=path$
ENDIF
REM Give the Filer time to see any !System or !Boot in the folder
SYS "OS_ReadMonotonicTime" TO pendat%
pendat%+=200
ENDPROC
