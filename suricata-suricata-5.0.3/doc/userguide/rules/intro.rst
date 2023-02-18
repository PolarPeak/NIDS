Rules Format
============

Signatures play a very important role in Suricata. In most occasions
people are using existing rulesets.

The official way to install rulesets is described in :doc:`../rule-management/suricata-update`.

This Suricata Rules document explains all about signatures; how to
read, adjust and create them.

A rule/signature consists of the following:

* The **action**, that determines what happens when the signature matches
* The **header**, defining the protocol, IP addresses, ports and direction of
  the rule.
* The **rule options**, defining the specifics of the rule.


.. role:: example-rule-action
.. role:: example-rule-header
.. role:: example-rule-options
.. role:: example-rule-emphasis

An example of a rule is as follows:

.. container:: example-rule

    :example-rule-action:`drop` :example-rule-header:`tcp $HOME_NET any -> $EXTERNAL_NET any` :example-rule-options:`(msg:"ET TROJAN Likely Bot Nick in IRC (USA +..)"; flow:established,to_server; flowbits:isset,is_proto_irc; content:"NICK "; pcre:"/NICK .*USA.*[0-9]{3,}/i"; reference:url,doc.emergingthreats.net/2008124; classtype:trojan-activity; sid:2008124; rev:2;)`

In this example, :example-rule-action:`red` is the action,
:example-rule-header:`green` is the header and :example-rule-options:`blue`
are the options.

We will be using the above signature as an example throughout
this section, highlighting the different parts of the signature. It is a
signature taken from the database of Emerging Threats, an open database
featuring lots of rules that you can freely download and use in your
Suricata instance.

Action
------
.. container:: example-rule

    :example-rule-emphasis:`drop` tcp $HOME_NET any -> $EXTERNAL_NET any (msg:"ET TROJAN Likely Bot Nick in IRC (USA +..)"; flow:established,to_server; flowbits:isset,is_proto_irc; content:"NICK "; pcre:"/NICK .*USA.*[0-9]{3,}/i"; reference:url,doc.emergingthreats.net/2008124; classtype:trojan-activity; sid:2008124; rev:2;)

For more information see :ref:`suricata-yaml-action-order`.


Protocol
--------
.. container:: example-rule

    drop :example-rule-emphasis:`tcp` $HOME_NET any -> $EXTERNAL_NET any (msg:"ET TROJAN Likely Bot Nick in IRC (USA +..)"; flow:established,to_server; flowbits:isset,is_proto_irc; content:"NICK "; pcre:"/NICK .*USA.*[0-9]{3,}/i"; reference:url,doc.emergingthreats.net/2008124; classtype:trojan-activity; sid:2008124; rev:2;)

This keyword in a signature tells Suricata which protocol it
concerns. You can choose between four basic protocols:

* tcp (for tcp-traffic)
* udp
* icmp
* ip (ip stands for 'all' or 'any')

There are also a few so-called application layer protocols, or layer 7 protocols
you can pick from. These are:

* http
* ftp
* tls (this includes ssl)
* smb
* dns
* dcerpc
* ssh
* smtp
* imap
* modbus (disabled by default)
* dnp3 (disabled by default)
* enip (disabled by default)
* nfs (depends on rust availability)
* ikev2 (depends on rust availability)
* krb5 (depends on rust availability)
* ntp (depends on rust availability)
* dhcp (depends on rust availability)

The availability of these protocols depends on whether the protocol is enabled in the configuration file suricata.yaml.

If you have a signature with for
instance a http protocol, Suricata makes sure the signature can only
match if it concerns http-traffic.

Source and destination
----------------------
.. container:: example-rule

    drop tcp :example-rule-emphasis:`$HOME_NET` any -> :example-rule-emphasis:`$EXTERNAL_NET` any (msg:"ET TROJAN Likely Bot Nick in IRC (USA +..)"; flow:established,to_server; flowbits:isset,is_proto_irc; content:"NICK "; pcre:"/NICK .*USA.*[0-9]{3,}/i"; reference:url,doc.emergingthreats.net/2008124; classtype:trojan-activity; sid:2008124; rev:2;)

*The first emphasized part is the source, the second is the destination (note the direction of the directional arrow).*

With source and destination, you specify the source of the traffic and the
destination of the traffic, respectively. You can assign IP addresses,
(both IPv4 and IPv6 are supported) and IP ranges. These can be combined with
operators:

==============  =========================
Operator        Description
==============  =========================
../..           IP ranges (CIDR notation)
!               exception/negation
[.., ..]        grouping
==============  =========================

Normally, you would also make use of variables, such as ``$HOME_NET`` and
``$EXTERNAL_NET``. The configuration file specifies the IP addresses these
concern, and these settings will be used in place of the variables in you rules.
See :ref:`suricata-yaml-rule-vars` for more information.

For example:

==================================  ==========================================
Example                             Meaning
==================================  ==========================================
! 1.1.1.1                           Every IP address but 1.1.1.1
![1.1.1.1, 1.1.1.2]                 Every IP address but 1.1.1.1 and 1.1.1.2
$HOME_NET                           Your setting of HOME_NET in yaml
[$EXTERNAL_NET, !$HOME_NET]         EXTERNAL_NET and not HOME_NET
[10.0.0.0/24, !10.0.0.5]            10.0.0.0/24 except for 10.0.0.5
[..., [....]]
[..., ![.....]]
==================================  ==========================================

.. warning::

   If you set your configuration to something like this::

       HOME_NET: any
       EXTERNAL_NET: ! $HOME_NET

   You can not write a signature using ``$EXTERNAL_NET`` because it stands for
   'not any'. This is an invalid setting.

Ports (source and destination)
------------------------------
.. container:: example-rule

    drop tcp $HOME_NET :example-rule-emphasis:`any` -> $EXTERNAL_NET :example-rule-emphasis:`any` (msg:"ET TROJAN Likely Bot Nick in IRC (USA +..)"; flow:established,to_server; flowbits:isset,is_proto_irc; content:"NICK "; pcre:"/NICK .*USA.*[0-9]{3,}/i"; reference:url,doc.emergingthreats.net/2008124; classtype:trojan-activity; sid:2008124; rev:2;)

*The first emphasized part is the source, the second is the destination (note the direction of the directional arrow).*

Traffic comes in and goes out through ports. Different ports have
different port numbers. For example, the default port for HTTP is 80 while 443 is
typically the port for HTTPS. Note, however, that the port does not
dictate which protocol is used in the communication. Rather, it determines which
application is receiving the data.

The ports mentioned above are typically the destination ports. Source ports,
i.e. the application that sent the packet, typically get assigned a random
port by the operating system. When writing a rule for your own HTTP service,
you would typically write ``any -> 80``, since that would mean any packet from
any source port to your HTTP application (running on port 80) is matched.

In setting ports you can make use of special operators as well, like
described above. Signs like:

==============  ==================
Operator        Description
==============  ==================
:               port ranges
!               exception/negation
[.., ..]        grouping
==============  ==================

For example:

==============  ==========================================
Example                             Meaning
==============  ==========================================
[80, 81, 82]    port 80, 81 and 82
[80: 82]        Range from 80 till 82
[1024: ]        From 1024 till the highest port-number
!80             Every port but 80
[80:100,!99]    Range from 80 till 100 but 99 excluded
[1:80,![2,4]]   Range from 1-80, except ports 2 and 4
[.., [..,..]]
==============  ==========================================


Direction
---------
.. container:: example-rule

    drop tcp $HOME_NET any :example-rule-emphasis:`->` $EXTERNAL_NET any (msg:"ET TROJAN Likely Bot Nick in IRC (USA +..)"; flow:established,to_server; flowbits:isset,is_proto_irc; content:"NICK "; pcre:"/NICK .*USA.*[0-9]{3,}/i"; reference:url,doc.emergingthreats.net/2008124; classtype:trojan-activity; sid:2008124; rev:2;)

The direction tells in which way the signature has to match. Nearly
every signature has an arrow to the right (``->``). This means that only
packets with the same direction can match. However, it is also possible to
have a rule match both ways (``<>``)::

  source -> destination
  source <> destination  (both directions)

.. warning::

   There is no 'reverse' style direction, i.e. there is no ``<-``.

The following example illustrates this. Say, there is a client with IP address
1.2.3.4 and port 1024, and a server with IP address 5.6.7.8, listening on port
80 (typically HTTP). The client sends a message to the server, and the server
replies with its answer.

.. image:: intro/TCP-session.png

Now, let's say we have a rule with the following header::

    alert tcp 1.2.3.4 1024 -> 5.6.7.8 80

Only the first packet will be matched by this rule, as the direction specifies
that we do not match on the response packet.

Rule options
------------
The rest of the rule consists of options. These are enclosed by parenthesis
and separated by semicolons. Some options have settings (such as ``msg``),
which are specified by the keyword of the option, followed by a colon,
followed by the settings. Others have no settings, and are simply the
keyword (such as ``nocase``)::

  <keyword>: <settings>;
  <keyword>;

Rule options have a specific ordering and changing their order would change the
meaning of the rule.

.. note::

    The characters ``;`` and ``"`` have special meaning in the
    Suricata rule language and must be escaped when used in a
    rule option value. For example::

	    msg:"Message with semicolon\;";

    As a consequence, you must also escape the backslash, as it functions
    as an escape character.

The rest of this chapter in the documentation documents the use of the various keywords.

Some generic details about keywords follow.

.. _rules-modifiers:

Modifier Keywords
~~~~~~~~~~~~~~~~~

Some keywords function act as modifiers. There are two types of modifiers.

* The older style **'content modifiers'** look back in the rule, e.g.::

      alert http any any -> any any (content:"index.php"; http_uri; sid:1;)

  In the above example the pattern 'index.php' is modified to inspect the HTTP uri buffer.

* The more recent type is called the **'sticky buffer'**. It places the buffer name first and all keywords following it apply to that buffer, for instance::

      alert http any any -> any any (http_response_line; content:"403 Forbidden"; sid:1;)

  In the above example the pattern '403 Forbidden' is inspected against the HTTP response line because it follows the ``http_response_line`` keyword.

.. _rules-normalized-buffers:

Normalized Buffers
~~~~~~~~~~~~~~~~~~
A packet consists of raw data. HTTP and reassembly make a copy of
those kinds of packets data. They erase anomalous content, combine
packets etcetera. What remains is a called the 'normalized buffer':

.. image:: normalized-buffers/normalization1.png

Because the data is being normalized, it is not what it used to be; it
is an interpretation.  Normalized buffers are: all HTTP-keywords,
reassembled streams, TLS-, SSL-, SSH-, FTP- and dcerpc-buffers.

Note that there are some exceptions, e.g. the ``http_raw_uri`` keyword.
See :ref:`rules-http-uri-normalization` for more information.
