#!/usr/bin/perl

use strict;
#use Test::More tests => 305;
use Test::More;
use FindBin qw($Bin);
use lib "$Bin/lib";
use MemcachedTest;

# assuming max slab is 1M and default mem is 64M
my $server = new_memcached();
my $sock = $server->sock;

# Skip this test on 32 bit due to bug 118, to re-enable this test
# comment out the the mem_stat and if block below and uncomment
# the Test::More test => 305 above
my $stats = mem_stats($sock);
if ($stats->{'pointer_size'} eq "32") {
    plan skip_all => 'Skipping LRU on 32-bit build (See bug 118 in bugzilla)';
    exit 0;
} else {
    plan tests => 305;
}

# create a big value for the largest slab
my $max = 1024 * 1024;
my $big = "a big value that's > .5M and < 1M. ";
while (length($big) * 2 < $max) {
    $big = $big . $big;
}

ok(length($big) > 512 * 1024);
ok(length($big) < 1024 * 1024);

# test that an even bigger value is rejected while we're here
my $too_big = $big . $big . $big;
my $len = length($too_big);
print $sock "set too_big 0 0 $len\r\n$too_big\r\n";
is(scalar <$sock>, "SERVER_ERROR object too large for cache\r\n", "too_big not stored");

# set the big value
my $len = length($big);
print $sock "set big 0 0 $len\r\n$big\r\n";
is(scalar <$sock>, "STORED\r\n", "stored big");
mem_get_is($sock, "big", $big);

# no evictions yet
my $stats = mem_stats($sock);
is($stats->{"evictions"}, "0", "no evictions to start");

# set many big items, enough to get evictions
for (my $i = 0; $i < 200; $i++) {
  print $sock "set item_$i 0 0 $len\r\n$big\r\n";
  is(scalar <$sock>, "STORED\r\n", "stored item_$i");
}

# some evictions should have happened
my $stats = mem_stats($sock);
my $evictions = int($stats->{"evictions"});
ok($evictions == 93, "some evictions happened");

# the first big value should be gone
mem_get_is($sock, "big", undef);

# the earliest items should be gone too
for (my $i = 0; $i < $evictions - 1; $i++) {
  mem_get_is($sock, "item_$i", undef);
}

# check that the non-evicted are the right ones
for (my $i = $evictions - 1; $i < $evictions + 4; $i++) {
  mem_get_is($sock, "item_$i", $big);
}
