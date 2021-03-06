#! perl
# Copyright (C) 2010, Parrot Foundation.

use strict;
use warnings;

use Test::More;
if (-e 'DEVELOPING' and ! -e 'Makefile') {
    plan tests =>  7;
}
else {
    plan skip_all =>
        q{Relevant only when working in checkout from repository and prior to configuration};
}
use Carp;
use Cwd;
use File::Copy;
use File::Path ();
use File::Temp qw| tempdir |;
use lib ( './lib' );

my $cwd = cwd();
{
    my $git_describe = "RELEASE_1_0_0-3-g35e41";
    my $tdir = tempdir( CLEANUP => 1 );
    ok( chdir $tdir, "Changed to temporary directory for testing" );
    my $libdir = qq{$tdir/lib};
    diag("using $libdir");
    ok( (File::Path::mkpath( [ $libdir ], 0, 0777 )), "Able to make libdir");
    local @INC;
    unshift @INC, $libdir;
    ok( (File::Path::mkpath( [ qq{$libdir/Parrot/Git} ], 0, 0777 )), "Able to make Parrot dir");
    ok( (copy qq{$cwd/lib/Parrot/Git/Describe.pm},
            qq{$libdir/Parrot/Git/Describe.pm}), "Able to copy Parrot::Git::Describe");
    my $cache = q{.parrot_current_git_describe};
    open my $FH, ">", $cache
        or croak "Unable to open $cache for writing";
    print $FH qq{$git_describe\n};
    close $FH or croak "Unable to close $cache after writing";

    require Parrot::Git::Describe;
    no warnings 'once';
    is($Parrot::Git::Describe::current, $git_describe,
        "Got expected git describe string from cache");
    use warnings;
    ok( chdir $cwd, "Able to change back to starting directory");
}

pass("Completed all tests in $0");

################### DOCUMENTATION ###################

=head1 NAME

065-git_describe_from_cache.t - test Parrot::Git::Describe

=head1 SYNOPSIS

    % prove t/configure/065-git_describe_from_cache.t

=head1 DESCRIPTION

The files in this directory test functionality used by F<Configure.pl>.

The tests in this file test Parrot::Git::Describe (F<lib/Parrot/Git/Describe.pm>).

=head1 AUTHOR

Jonathan "Duke" Leto

=head1 SEE ALSO

Parrot::Configure, F<Configure.pl>.

=cut

# Local Variables:
#   mode: cperl
#   cperl-indent-level: 4
#   fill-column: 100
# End:
# vim: expandtab shiftwidth=4:
