Datasets
========

.. warning:: This is an experimental feature. Syntax and functionality may
             change in future releases.

Using the ``dataset`` and ``datarep`` keyword it is possible to match on
large amounts of data against any sticky buffer.

For example, to match against a dns black list called ``dns-bl``::

    dns.query; dataset:isset,dns-bl;

These keywords are aware of transforms. So to look up a DNS query against
a MD5 black list::

    dns.query; to_md5; dataset:isset,dns-bl;

Global config (optional)
------------------------

Datasets can optionally be defined in the main config. Sets can also be
declared from the rule syntax.

Example of sets for tracking unique values::

    datasets:
      ua-seen:
        type: string
        state: ua-seen.lst
      dns-sha256-seen:
        type: sha256
        state: dns-sha256-seen.lst

Rules to go with the above:

.. container:: example-rule

    alert dns any any -> any any (msg:"dns list test"; dns.query; to_sha256; dataset:isset,dns-sha256-seen; sid:123; rev:1;)

.. container:: example-rule

    alert http any any -> any any (msg: "http user-agent test"; http.user_agent; dataset:set,ua-seen; sid:234; rev:1;)

Rule keywords
-------------

dataset
~~~~~~~

Datasets are binary: something is in the set or it's not.

Syntax::

    dataset:<cmd>,<name>,<options>;

    dataset:<set|isset|isnotset>,<name> \
        [, type <string|md5|sha256>, save <file name>, load <file name>, state <file name>];

type <type>
  the data type: string, md5, sha256
save <file name>
  file name for saving the in-memory data when Suricata exits
load <file name>
  file name for load the data when Suricata starts up
state
  sets both 'save' and 'load' to the same value

.. note:: 'load' or 'load' plus 'save' and 'state' cannot be mixed.

datarep
~~~~~~~

Data Reputation allows matching data against a reputation list.

Syntax::

    datarep:<name>,<operator>,<value>, \
        [, load <file name>, type <string|md5|sha256>];

Example rules could look like::

    alert dns any any -> any any (dns.query; to_md5; datarep:dns_md5, >, 200, load dns_md5.rep, type md5; sid:1;)
    alert dns any any -> any any (dns.query; to_sha256; datarep:dns_sha256, >, 200, load dns_sha256.rep, type sha256; sid:2;)
    alert dns any any -> any any (dns.query; datarep:dns_string, >, 200, load dns_string.rep, type string; sid:3;)

In these examples the DNS query string is checked against three different
reputation lists. A MD5 list, a SHA256 list, and a raw string (buffer) list.
The rules will only match if the data is in the list and the reputation
value is higher than 200.


Unix Socket
-----------

dataset-add
~~~~~~~~~~~

Unix Socket command to add data to a set. On success, the addition becomes
active instantly.

Syntax::

    dataset-add <set name> <set type> <data>

set name
  Name of an already defined dataset
type
  Data type: string, md5, sha256
data
  Data to add in serialized form (base64 for string, hex notation for md5/sha256)

Example adding 'google.com' to set 'myset'::

    dataset-add myset string Z29vZ2xlLmNvbQ==


File formats
------------

Datasets use a simple CSV format where data is per line in the file.

data types
~~~~~~~~~~

string
  in the file as base64 encoded string
md5
  in the file as hex encoded string
sha256
  in the file as hex encoded string


dataset
~~~~~~~

Datasets have a simple structure, where there is one piece of data
per line in the file.

Syntax::

    <data>

e.g. for ua-seen with type string::

    TW96aWxsYS80LjAgKGNvbXBhdGlibGU7ICk=

which when piped to ``base64 -d`` reveals its value::

    Mozilla/4.0 (compatible; )


datarep
~~~~~~~

The datarep format follows the dataset, expect that there are 1 more CSV
field:

Syntax::

    <data>,<value>
