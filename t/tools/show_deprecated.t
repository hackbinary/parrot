#! perl
# Copyright (C) 2011, Parrot Foundation.

=head1 NAME

t/tools/show_deprecated.pl.t - test show_deprecated.pl

=head1 SYNOPSIS

    % prove t/tools/show_deprecated.t - test show_deprecated.pl

=head1 DESCRIPTION

Tests the C<show_deprecated.pl> tool. This also serves as a basic
test that api.yaml is valid YAML.

These tests could be greatly improved.

=cut

use strict;
use warnings;
use lib qw(lib);

use Test::More;
use File::Spec::Functions;

eval {require YAML; YAML->import('LoadFile')};
if ($@) {
   plan(skip_all => "YAML.pm required for this test.");
}

# List::MoreUtils.pm is also used by show_deprecated.pl
eval {require List::MoreUtils; List::MoreUtils->import('any')};
if ($@) {
   plan(skip_all => "List::MoreUtils.pm required for this test.");
}

plan tests => 1;

my $script = catfile(qw/tools dev show_deprecated.pl/);

my $exit_code = system("$^X $script");
ok(!$exit_code, "show_deprecated.pl can read api.yaml and exits successfully");

# Local Variables:
#   mode: cperl
#   cperl-indent-level: 4
#   fill-column: 100
# End:
# vim: expandtab shiftwidth=4:
