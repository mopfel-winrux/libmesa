::  mesa-test: interop test agent for libmesa
::
::    pokes (%noun):
::      %crash      crash (the poke is nacked)
::      %poke-back  ack, then poke the sender's %libmesa agent back
::      [%idle dude]    suspend an agent (gall %idle)
::      [%revive desk]  restart a desk's agents (clay %zest, as |revive)
::      %germ       make /secret a %shut coop, with /secret/x 'treasure'
::      [%shut-read ship path]  %keen a %shut path of theirs (gall asks
::                              them for the key over /gk first)
::      anything    ack, and give a fact on /updates
::
::    watch /updates: an immediate fact, then one per poke
::    remote scry: /g/x/1/mesa-test//1/hello is 'world'; after %germ,
::    /g/x/1/mesa-test//1/secret/x is 'treasure', %shut
::
/+  default-agent
|%
+$  card  card:agent:gall
--
=|  pokes=@ud
^-  agent:gall
|_  =bowl:gall
+*  this  .
    def   ~(. (default-agent this %|) bowl)
::
++  on-init
  :_  this
  [%pass /grow %grow /hello [%noun 'world']]~
::
++  on-save  !>(pokes)
++  on-load  |=(v=vase `this(pokes !<(@ud v)))
::
++  on-poke
  |=  [=mark =vase]
  ^-  (quip card _this)
  ~&  [%mesa-test-poke src.bowl mark q.vase]
  ?:  =(%crash q.vase)  ~|(%mesa-test-crash !!)
  ?:  ?=([%idle @] q.vase)
    :_  this
    [%pass /idle %arvo %g %idle ;;(dude:gall +.q.vase)]~
  ?:  ?=([%revive @] q.vase)
    :_  this
    [%pass /revive %arvo %c %zest ;;(desk +.q.vase) %live]~
  ?:  =(%germ q.vase)
    :_  this
    :~  [%pass /germ %germ /secret]
        [%pass /tend %tend /secret /x [%noun 'treasure']]
    ==
  ?:  ?=([%shut-read @ *] q.vase)
    =+  ;;([her=@p pax=path] +.q.vase)
    :_  this
    [%pass /shut %keen & her pax]~
  =.  pokes  +(pokes)
  :_  this
  :-  [%give %fact ~[/updates] [%noun !>([%poked pokes q.vase])]]
  ?.  =(%poke-back q.vase)  ~
  [%pass /back %agent [src.bowl %libmesa] %poke [%noun !>([%back pokes])]]~
::
++  on-watch
  |=  =path
  ^-  (quip card _this)
  ?.  ?=([%updates ~] path)  (on-watch:def path)
  ~&  [%mesa-test-watch src.bowl]
  :_  this
  [%give %fact ~ [%noun !>([%hello src.bowl])]]~
::
++  on-leave
  |=  =path
  ~&  [%mesa-test-leave src.bowl path]
  `this
::
++  on-agent
  |=  [=wire =sign:agent:gall]
  ^-  (quip card _this)
  ~&  [%mesa-test-agent wire -.sign ?:(?=(%poke-ack -.sign) ?=(~ p.sign) %.y)]
  `this
::
++  on-peek
  |=  =path
  ^-  (unit (unit cage))
  ::  anyone may have the key to /secret
  ?:  ?=([%c %secret @ ~] path)  ``noun+!>(&)
  (on-peek:def path)
++  on-arvo
  |=  [=wire sign=sign-arvo]
  ^-  (quip card _this)
  ~&  [%mesa-test-arvo wire ?.(?=([%ames %sage *] sign) -.+.sign sign)]
  `this
++  on-fail   on-fail:def
--
