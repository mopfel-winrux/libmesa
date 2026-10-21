::  mesa-test: interop test agent for libmesa
::
::    pokes (%noun):
::      %crash      crash (the poke is nacked)
::      %poke-back  ack, then poke the sender's %libmesa agent back
::      anything    ack, and give a fact on /updates
::
::    watch /updates: an immediate fact, then one per poke
::    remote scry: /g/x/1/mesa-test//1/hello is 'world'
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
++  on-peek   on-peek:def
++  on-arvo   on-arvo:def
++  on-fail   on-fail:def
--
