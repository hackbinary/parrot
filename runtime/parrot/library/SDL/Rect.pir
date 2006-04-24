
# $Id$

=head1 NAME

SDL::Rect - Parrot class representing rectangles in Parrot SDL

=head1 SYNOPSIS

	# load this library
	load_bytecode 'library/SDL/Rect.pir'

	# create a new SDL::Rect object
	.local pmc rect
	.local int rect_type

	find_type rect_type, 'SDL::Rect'
	rect = new rect_type

	# set some arguments for this SDL::Rect
	.local pmc rect_args

	new rect_args, .Hash
	rect_args[ 'x'      ] = 270
	rect_args[ 'y'      ] = 190
	rect_args[ 'height' ] = 100
	rect_args[ 'width'  ] = 100

	# now set the arguments on the object
	rect.'_new'( rect_args )

	# ... and blit to or fill surfaces with this object!

=head1 DESCRIPTION

The SDL::Rect class represents rects in SDL.  SDL::Rect objects represent
sources and destinations for filling and blitting to and from SDL::Surface
objects. 

=head1 METHODS

An SDL::Rect object has the following methods:

=over 4

=cut

.namespace [ 'SDL::Rect' ]

.sub _initialize :load

	$I0 = find_type 'SDL::Rect'
	if $I0 > 1 goto END
	
	.local   pmc rect_class
	newclass     rect_class, 'SDL::Rect'
	addattribute rect_class, '_rect'

	.local pmc initializer
	new initializer, .String
	initializer = '_new'
	setprop      rect_class, 'BUILD', initializer
END:

.end

=item _new( rect_args )

Given a C<Hash> of arguments, sets the attributes of this object.  The hash
has the following keys:

=over 4

=item x

The x coordinate of this object, in pixels.  This represents a position within
a SDL::Surface object, but it's an attribute of the rect, not the surface.

=item y

The y coordinate of this object, in pixels.  This represents a position within
a SDL::Surface object, but it's an attribute of the rect, not the surface.

=item height

The height, in pixels, of this object.

=item width

The width, in pixels, of this object.

=back

The name of this method may change, as per discussion on p6i.

=cut

.sub _new method
	.param pmc args :optional
	.param int argc :opt_flag

	.local pmc  fetch_layout
	find_global fetch_layout, 'SDL::NCI', 'fetch_layout'

	.local pmc layout
	layout = fetch_layout( 'Rect' )

	.local pmc rect
	new rect, .ManagedStruct, layout

	.local int height
	.local int width
	.local int x
	.local int y

	if argc == 0 goto ZEROS
	
	set height, args['height']
	set rect['height'], height

	set width, args['width']
	set rect['width'], width

	set x, args['x']
	set rect['x'], x

	set y, args['y']
	set rect['y'], y
	branch DONE

ZEROS:
	set rect['height'], 0
	set rect['width'], 0
	set rect['x'], 0
	set rect['y'], 0

DONE:
	.local int offset
	classoffset  offset,   self, 'SDL::Rect'
	setattribute   self, offset, rect

.end

=item rect()

This method returns the underlying C<SDL_Rect>.  You'll probably never need to
call this directly, unless you're using raw SDL functions.

=cut

.sub rect method
	.local pmc rect 
	.local int offset

	classoffset  offset, self, 'SDL::Rect'
	getattribute   rect, self, offset

	.pcc_begin_return
		.return rect
	.pcc_end_return
.end

=item height()

Returns the height of this object in pixels.  Arguably, this should also set
the value.

=cut

.sub height method
	.param int new_height     :optional
	.param int has_new_height :opt_flag

	.local pmc rect 
	.local int result

	rect             = self.'rect'()

	unless has_new_height goto getter
	rect[ 'height' ] = new_height

getter:
	result           = rect[ 'height' ]

	.pcc_begin_return
		.return result
	.pcc_end_return
.end

=item width()

Returns the width of this object in pixels.  Arguably, this should also set the
value.

=cut

.sub width method
	.param int new_width     :optional
	.param int has_new_width :optional

	.local pmc rect 
	.local int result

	rect            = self.'rect'()

	unless has_new_width goto getter
	rect[ 'width' ] = new_width

getter:
	result          = rect[ 'width' ]

	.pcc_begin_return
		.return result
	.pcc_end_return
.end

=item x( [ new_x_coordinate ] )

Gets and sets the x coordinate of this rect.  Note that this value is always an
integer.

=cut

.sub x method
	.param int new_x     :optional
	.param int has_new_x :opt_flag

	.local pmc rect 

	rect           = self.'rect'()

	unless has_new_x goto getter
	rect[ 'x' ]    = new_x

getter:
	.local int result
	result         = rect[ 'x' ]

	.return( result )
.end

=item y( [ new_y_coordinate ] )

Gets and sets the y coordinate of this rect.  Note that this value is always an
integer.

=cut

.sub y method
	.param int new_y     :optional
	.param int has_new_y :opt_flag

	.local pmc rect 

	rect           = self.'rect'()

	unless has_new_y goto _getter
	rect[ 'y' ]    = new_y

_getter:
	.local int result
	result         = rect[ 'y' ]

	.pcc_begin_return
		.return result
	.pcc_end_return
.end

=back

=head1 AUTHOR

Written and maintained by chromatic, E<lt>chromatic at wgz dot orgE<gt>, with
suggestions from Jens Rieks.  Please send patches, feedback, and suggestions to
the Perl 6 Internals mailing list.

=head1 COPYRIGHT

Copyright (c) 2004-2006, The Perl Foundation.

=cut
