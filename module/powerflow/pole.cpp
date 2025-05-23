// powerflow/pole.cpp
// Copyright (C) 2018, Regents of the Leland Stanford Junior University
//
// Processing sequence for pole failure analysis:
//
// Precommit (random):
//  - pole          update weather/degredation/resisting moment
//  - pole_mount    get initial equipment status
//
// Presync (top-down):
//  - pole          initialize moment accumulators,
//  - pole_mount    set interim equipment status
//
// Sync (bottom-up)
//  - pole_mount    update moment accumulators
//  - pole          (nop)
//
// Postsync (top-down):
//  - pole          calculate total moment and failure status
//  - pole_mount    set interim equipment status
//
// Commit (random):
//  - pole          finalize pole status
//  - pole_mount    finalize equipment status
//

#include "powerflow.h"
#include "math.h"
#include <typeinfo>
#include <iostream>


EXPORT_CREATE(pole)
EXPORT_INIT(pole)
EXPORT_PRECOMMIT(pole)
EXPORT_SYNC(pole)
EXPORT_COMMIT(pole)

CLASS *pole::oclass = NULL;
CLASS *pole::pclass = NULL;
pole *pole::defaults = NULL;

static char32 wind_speed_name = "wind_speed";
static char32 wind_dir_name = "wind_dir";
static char32 wind_gust_name = "wind_gust";
static double default_repair_time = 24.0;
static bool stop_on_pole_failure = false;

pole::pole(MODULE *mod)
{
	if ( oclass == NULL )
	{
		pclass = node::oclass;
		oclass = gl_register_class(mod,"pole",sizeof(pole),PC_PRETOPDOWN|PC_POSTTOPDOWN|PC_UNSAFE_OVERRIDE_OMIT|PC_AUTOLOCK);
		if ( oclass == NULL )
			throw "unable to register class pole";
		oclass->trl = TRL_PROTOTYPE;
		if ( gl_publish_variable(oclass,

			PT_enumeration, "status", get_pole_status_offset(),
                PT_KEYWORD, "OK", (enumeration)PS_OK,
                PT_KEYWORD, "FAILED", (enumeration)PS_FAILED,
                PT_DEFAULT, "OK",
                PT_DESCRIPTION, "pole status",

            PT_double, "tilt_angle[deg]", get_tilt_angle_offset(),
                PT_DEFAULT, "0.0 deg",
                PT_DESCRIPTION, "tilt angle of pole from the centerline",

            PT_double, "tilt_direction[deg]", get_tilt_direction_offset(),
                PT_DEFAULT, "0.0 deg",
                PT_DESCRIPTION, "tilt direction of pole",

            PT_object, "weather", get_weather_offset(),
                PT_DESCRIPTION, "weather data",

            PT_object, "configuration", get_configuration_offset(),
                PT_REQUIRED,
                PT_DESCRIPTION, "configuration data",

            PT_int32, "install_year", get_install_year_offset(),
                PT_REQUIRED,
                PT_DESCRIPTION, "the year of pole was installed",

            PT_double, "repair_time[h]", get_repair_time_offset(),
                PT_DESCRIPTION, "typical repair time after pole failure",

            PT_double, "wind_speed[m/s]", get_wind_speed_offset(),
                PT_DEFAULT, "0 m/s",
                PT_DESCRIPTION, "local wind speed",

            PT_double, "wind_direction[deg]", get_wind_direction_offset(),
                PT_DEFAULT, "0 deg",
                PT_DESCRIPTION, "local wind direction",

            PT_double, "wind_gusts[m/s]", get_wind_gusts_offset(),
                PT_DEFAULT, "0 m/s",
                PT_DESCRIPTION, "local wind gusts",

            PT_double, "pole_stress[pu]", get_pole_stress_offset(),
                PT_OUTPUT,
                PT_DESCRIPTION, "ratio of actual stress to critical stress",

            PT_double, "pole_stress_polynomial_a[ft*lb]", get_pole_stress_polynomial_a_offset(),
                PT_OUTPUT,
                PT_DESCRIPTION, "constant a of the pole stress polynomial function",
            PT_double, "pole_stress_polynomial_b[ft*lb]", get_pole_stress_polynomial_b_offset(),
                PT_OUTPUT,
                PT_DESCRIPTION, "constant b of the pole stress polynomial function",
            PT_double, "pole_stress_polynomial_c[ft*lb]", get_pole_stress_polynomial_c_offset(),
                PT_OUTPUT,
                PT_DESCRIPTION, "constant c of the pole stress polynomial function",

            PT_double, "susceptibility[pu.s/m]", get_susceptibility_offset(),
                PT_OUTPUT,
                PT_DESCRIPTION, "susceptibility of pole to wind stress (derivative of pole stress w.r.t wind speed)",

            PT_double, "total_moment[ft*lb]", get_total_moment_offset(),
                PT_OUTPUT,
                PT_DESCRIPTION, "the total moment on the pole",

            PT_double, "resisting_moment[ft*lb]", get_resisting_moment_offset(),
                PT_OUTPUT,
                PT_DESCRIPTION, "the resisting moment on the pole",

            PT_double, "pole_moment[ft*lb]", get_pole_moment_offset(),
                PT_OUTPUT,
                PT_DESCRIPTION, "the moment of the pole",

            PT_double, "pole_moment_per_wind[ft*lb]", get_pole_moment_per_wind_offset(),
                PT_OUTPUT,
                PT_DESCRIPTION, "the moment of the pole without wind",

            PT_double, "equipment_moment[ft*lb]", get_equipment_moment_offset(),
                PT_OUTPUT,
                PT_DESCRIPTION, "the moment of the equipment",

            PT_double, "equipment_weight[ft*lb]", get_equipment_weight_offset(),
                PT_OUTPUT,
                PT_DESCRIPTION, "the moment of the equipment weight",

            PT_double, "equipment_moment_nowind[ft*lb]", get_equipment_moment_nowind_offset(),
                PT_OUTPUT,
                PT_DESCRIPTION, "the moment of the equipment without wind",

            PT_double, "critical_wind_speed[m/s]", get_critical_wind_speed_offset(),
                PT_OUTPUT,
                PT_DESCRIPTION, "wind speed at pole failure",

            PT_double, "wire_weight[ft*lb]", get_wire_weight_offset(),
                PT_OUTPUT,
                PT_DESCRIPTION, "wire moment due to conductor weight",

            PT_double, "wire_moment_x[ft*lb]", get_wire_moment_x_offset(),
                PT_OUTPUT,
                PT_DESCRIPTION, "wire moment in x-axis due to tension and wind load",

            PT_double, "wire_moment_y[ft*lb]", get_wire_moment_y_offset(),
                PT_OUTPUT,
                PT_DESCRIPTION, "wire moment in y-axis due to tension and wind load",

            PT_double, "equipment_moment_x[ft*lb]", get_wire_moment_x_offset(),
                PT_OUTPUT,
                PT_DESCRIPTION, "equipment moment in x-axis due to wind load",

            PT_double, "equipment_moment_y[ft*lb]", get_wire_moment_y_offset(),
                PT_OUTPUT,
                PT_DESCRIPTION, "equipment moment in y-axis due to wind load",

            PT_double, "wire_tension[ft*lb]", get_wire_tension_offset(),
                PT_OUTPUT,
                PT_DESCRIPTION, "wire moment due to conductor tension",

            PT_double, "guy_height[ft]", get_guy_height_offset(),
                PT_DEFAULT, "0 ft",
                PT_DESCRIPTION, "guy wire attachment height",

            NULL) < 1 ) throw "unable to publish properties in " __FILE__;
		gl_global_create("powerflow::repair_time[h]",PT_double,&default_repair_time,NULL);
        gl_global_create("powerflow::wind_speed_name",PT_char32,&wind_speed_name,NULL);
        gl_global_create("powerflow::wind_dir_name",PT_char32,&wind_dir_name,NULL);
        gl_global_create("powerflow::wind_gust_name",PT_char32,&wind_gust_name,NULL);
        gl_global_create("powerflow::stop_on_pole_failure",PT_bool,&stop_on_pole_failure,NULL);
	}
}

double wind_gust_cdf(double wind_ratio)
{
  double L, K, w;
  double x = log(wind_ratio);
  double const a1 = 0.31938153, a2 = -0.356563782, a3 = 1.781477937;
  double const a4 = -1.821255978, a5 = 1.330274429;
  L = fabs(x);
  K = 1.0 / (1.0 + 0.2316419 * L);
  w = 1.0 / sqrt(2 * 3.1415) * exp(-L *L / 2) * (a1 * K + a2 * K *K + a3 * pow(K,3) + a4 * pow(K,4) + a5 * pow(K,5));
  if (x < 0)
  {
    w = 1 - w;
  }
  return w;
}

// Effective pole height: 
// height above the ground minus guy wire height (how far it extends without support, a.k.a. from where it would snap)
double pole::calc_height() {
   return config->pole_length - config->pole_depth - guy_height; 
}

// Diameter at effective pole height, using linear interpolation from pole top diameter and ground diameter
// diameter = top diameter + height difference from top * (change in diameter / change in height)
double pole::calc_diameter() {
    return config->top_diameter 
            + height*(config->ground_diameter-config->top_diameter)
	                /(config->pole_length - config->pole_depth);
}

// Pole moment per unit of wind pressure
// Equation comes from Pole Loading Model, pdf linked in docs.
//    moment   =             force                    *           radius    
// Pole moment = wind pressure * cross-sectional area * height from supported point
// Calculated by approximating the pole's cross sectional area as a very tall trapezoid (diameter at the bottom 
//    is larger than that at the top). Integrating to sum up the contribution to moment from every slice 
//    of area at a given height gives:
//    pole moment = 1/6 W H^2 (D_base + 2*D_top)
//    where W = wind pressure, dropped from the equation to be multiplied in later
//          H = pole height
//          D = diameter at the supported base and at the top
//          * 1/12 to convert diameter from inches to feet
//          * Overload capacity factor increases the modeled force on the pole. 
//            For reliability, the National Electrical Safety Code requires that these overload factors 
//            be used when calculating the maximum force a pole must withstand.
double pole::calc_pole_moment_per_wind() {
    return  height * height * (diameter + 2*config->top_diameter)/72.0 * config->overload_factor_transverse_general;
}

void pole::reset_commit_accumulators()
{
	equipment_moment_nowind = 0.0;
	wire_load_nowind = 0.0;
	wire_moment_nowind = 0.0;
    pole_moment = 0.0;
    pole_moment_wind = 0.0;
    equipment_moment = 0.0;
    wire_weight = 0.0;
    wire_moment_x = 0.0;
    wire_moment_y = 0.0;
    wire_tension = 0.0;
    equipment_moment_x = 0.0;
    equipment_moment_y = 0.0;
}

void pole::reset_sync_accumulators()
{
    wire_wind = 0.0;
}

int pole::create(void)
{
	configuration = NULL;
	is_deadend = FALSE;
	config = NULL;
	last_wind_speed = 0.0;
	last_wind_speed = 0.0;
	down_time = TS_NEVER;
	current_hollow_diameter = 0.0;
    total_moment = 0.0;
    wind_pressure = 0.0;
    pole_stress = 0.0;
    critical_wind_speed = 0.0;
    susceptibility = 0.0;
    pole_moment_per_wind = 0.0;
    reset_commit_accumulators();
    reset_sync_accumulators();
    wind_speed_ref = NULL;
    wind_direction_ref = NULL;
    wind_gusts_ref = NULL;
	return 1;
}

int pole::init(OBJECT *parent)
{
	// configuration
	if ( configuration == NULL || ! gl_object_isa(configuration,"pole_configuration") )
	{
		error("configuration is not set to a pole_configuration object");
		return 0;
	}
	config = OBJECTDATA(configuration,pole_configuration);
    verbose("configuration = %s",configuration->name);

	if ( repair_time <= 0.0 )
	{
		double *pRepairTime = (double*)gl_get_addr(configuration, "repair_time");
		if ( pRepairTime && *pRepairTime > 0 )
		{
			repair_time = *pRepairTime;
		}
		else if ( default_repair_time > 0 )
		{
			repair_time = default_repair_time;
		}
		else
		{
			exception("pole::default_repair_time must be positive");
		}

    }
    verbose("repair_time = %g hr",repair_time);
    // weather check
    if ( weather )
    {
        wind_speed_ref = new gld_property(weather,(const char*)wind_speed_name);
        if ( ! wind_speed_ref->is_valid() )
        {
            warning("weather data does not include %s, using local wind %s data only",(const char*)wind_speed_name,"speed");
            delete wind_speed_ref;
            wind_speed_ref = NULL;
        }
        else if ( wind_speed != 0.0 )
        {
            warning("weather data will overwrite local wind %s data","speed");
        }
        else
        {
            verbose("wind_speed = %g m/s (ref '%s')", wind_speed, weather->name);
        }

        wind_direction_ref = new gld_property(weather,(const char*)wind_dir_name);
        if ( ! wind_direction_ref->is_valid() )
        {
            warning("weather data does not include %s, using local wind %s data only",(const char*)wind_dir_name,"direction");
            delete wind_direction_ref;
            wind_direction_ref = NULL;
        }
        else if ( wind_direction != 0.0 )
        {
            warning("weather data will overwrite local wind %s data","direction");
        }
        else
        {
            verbose("wind_direction = %g deg (ref '%s')", wind_direction, weather->name);
        }

        wind_gusts_ref = new gld_property(weather,(const char*)wind_gust_name);
        if ( ! wind_gusts_ref->is_valid() )
        {
            warning("weather data does not include %s, using local wind %s data only",(const char*)wind_gust_name,"gusts");
            delete wind_gusts_ref;
            wind_gusts_ref = NULL;
        }
        else if ( wind_gusts != 0.0 )
        {
            warning("weather data will overwrite local wind %s data","gusts");
        }
        else
        {
            verbose("wind_gusts = %g m/s (ref '%s')", wind_gusts, weather->name);
        }
    }

	// tilt
	if ( tilt_angle < 0 || tilt_angle > 90 )
	{
		error("pole tilt angle is not between and 0 and 90 degrees");
		return 0;
	}
    verbose("tilt_angle = %g deg",tilt_angle);
	if ( tilt_direction < 0 || tilt_direction >= 360 )
	{
		error("pole tilt direction is not between 0 and 360 degrees");
		return 0;
	}
    verbose("tilt_direction = %g deg",tilt_direction);

    // effective pole height
    // height above the ground minus guy wire height (how far it extends without support, a.k.a. from where it would snap)
    height = calc_height();
    verbose("height = %g ft",height);

    // diameter at effective pole height
    diameter = calc_diameter();
	
    // calculation resisting moment
    resisting_moment = 0.008186
		* config->strength_factor_250b_wood
		* config->fiber_strength
		* ( diameter * diameter * diameter);
    verbose("resisting_moment = %.0f ft*lb (not aged)",resisting_moment);

    // pole moment per unit of wind pressure
    pole_moment_per_wind = calc_pole_moment_per_wind();
    verbose("pole_moment_per_wind = %g ft*lb (wind load is 1 lb/sf)",pole_moment_per_wind);

    // check install year
	if ( install_year > gl_globalclock )
		warning("pole install year in the future are assumed to be current time");
    verbose("install_year = %d",(int)install_year);
	return 1;
}

TIMESTAMP pole::precommit(TIMESTAMP t0)
{
    reset_commit_accumulators();

    // wind data
    if ( wind_speed_ref )
    {
        wind_speed = wind_speed_ref->get_double();
    }
    if ( wind_direction_ref )
    {
        wind_direction = wind_direction_ref->get_double();
    }
    if ( wind_gusts_ref )
    {
        wind_gusts = wind_gusts_ref->get_double();
    }
    height = calc_height(); // effective pole height (length of unsupported pole)
    double t0_year = 1970 + (int)(t0/86400/365.24);
	double age = t0_year - install_year;
    if ( age > 0 && config->degradation_rate > 0 )
	{
		if ( age > 0 )
			current_hollow_diameter = 2.0*age*config->degradation_rate*diameter/config->ground_diameter;
		else
			current_hollow_diameter = 0.0; // ignore future installation years
        verbose("current_hollow_diameter = %g in",current_hollow_diameter);
    }
    else
    {
        verbose("pole degradation model disabled (age=%g, degradation_rate=%g)", age,config->degradation_rate);
    }

    // update resisting moment considering aging
    resisting_moment = 0.008186 // constant * pi^3
        * config->strength_factor_250b_wood
        * config->fiber_strength
        * (diameter*diameter*diameter-current_hollow_diameter*current_hollow_diameter*current_hollow_diameter);
    verbose("updated resisting moment %.0f ft*lb (aged)",resisting_moment);

    if ( pole_status == PS_FAILED && (gl_globalclock-down_time)/3600.0 > repair_time )
	{
        verbose("pole repair time has arrived");
		tilt_angle = 0.0;
		tilt_direction = 0.0;
		pole_status = PS_OK;
		install_year = 1970 + (unsigned int)(t0/86400/365.24);
        verbose("install_year = %d (pole repaired)", install_year);

        recalc = true;
        verbose("setting pole recalculation flag");
	}
	else if ( pole_status == PS_OK && (last_wind_speed != wind_speed || critical_wind_speed == 0.0))
	{
        if ( resisting_moment < 0 )
        {
            warning("pole has degraded past point of static failure");;
            resisting_moment = 0.0;
        }
        verbose("wind speed change requires update of pole analysis");
        verbose("wind speed = %g m/s", wind_speed);
        wind_pressure = 0.00256 * (2.24*wind_speed) * (2.24*wind_speed); // 2.24 account for m/s to mph conversion
        verbose("wind_pressure = %g psf",wind_pressure); // unit: pounds per square foot

        if ( tilt_angle > 0.0 )
        {
            const double D1 = config->top_diameter/12;
            const double D0 = (diameter-current_hollow_diameter)/12;
            const double rho = config->material_density;
            pole_moment += 0.125 * rho * PI * height * height * sin(tilt_angle/180*PI) * ((D0+D1)*(D0+D1)/6 + D1*D1/3);
        }
        verbose("pole_moment = %g ft*lb (tilt moment)",pole_moment);

        // TODO: this needs to be moved to commit in order to consider equipment and wire wind susceptibility
        pole_moment_per_wind = calc_pole_moment_per_wind();
        verbose("pole_moment_per_wind = %g ft*lb (wind load is 1 lb/sf)",pole_moment_per_wind);
        
        last_wind_speed = wind_speed;
        if ( wind_pressure > 0.0 )
        {
            pole_moment_wind = wind_pressure * pole_moment_per_wind;
            verbose("pole_moment_wind = %g ft*lb",pole_moment_wind);
        }

        recalc = true;
        verbose("setting pole recalculation flag");
    }

    // next event
    return TS_NEVER;
}

TIMESTAMP pole::presync(TIMESTAMP t0)
{
    if ( recalc )
    {
        reset_sync_accumulators();
    }
	return TS_NEVER;
}

TIMESTAMP pole::sync(TIMESTAMP t0)
{
    //  - pole          (nop)
	return TS_NEVER;
}

TIMESTAMP pole::postsync(TIMESTAMP t0) ////
{
    //  - pole          calculate total moment and failure status
    if ( recalc )
    {
        verbose("wire_moment_x = %g ft*lb (moment in x-axis)", wire_moment_x);
        verbose("wire_moment_y = %g ft*lb (moment in y-axis)", wire_moment_y);

        verbose("equipment_moment_x = %g ft*lb (moment in x-axis)", equipment_moment_x);
        verbose("equipment_moment_y = %g ft*lb (moment in y-axis)", equipment_moment_y);

        double pole_moment_x = pole_moment_wind*cos(wind_direction*PI/180)+pole_moment*cos(tilt_direction*PI/180);
        double pole_moment_y = pole_moment_wind*sin(wind_direction*PI/180)+pole_moment*sin(tilt_direction*PI/180);
        verbose("pole_moment_x = %g ft*lb (moment in x-axis)", pole_moment_x);
        verbose("pole_moment_y = %g ft*lb (moment in y-axis)", pole_moment_y);

        total_moment = sqrt(
            (wire_moment_x+equipment_moment_x+pole_moment_x)*(wire_moment_x+equipment_moment_x+pole_moment_x) +
            (wire_moment_y+equipment_moment_y+pole_moment_y)*(wire_moment_y+equipment_moment_y+pole_moment_y));
        verbose("total_moment = %g ft*lb\n",total_moment);
        verbose("resisting_moment = %g ft*lb\n",resisting_moment);

        if ( resisting_moment > 0.0 ) 
        {
            pole_stress = total_moment/resisting_moment;
        }
        else
        {
            pole_stress = INFINITY;
        }
        verbose("name is %s ################################\n", my()->name);
        verbose("pole_stress = %g %%\n",pole_stress*100);
        if ( wind_speed > 0.0 )
            susceptibility = (cos(wind_direction*PI/180) + sin(wind_direction*PI/180)) * (pole_moment_per_wind+equipment_moment_nowind+wire_moment_nowind) * (
                (wire_moment_x+equipment_moment_x+pole_moment_x) + (wire_moment_y+equipment_moment_y+pole_moment_y)
                ) * (0.00256*2*2.24*wind_speed*2.24) / total_moment;
		else
			susceptibility = 0.0;
        verbose("susceptibility = %g ft*lb.s/m\n",susceptibility);
        verbose("wind_speed = %g m/s", wind_speed);
        verbose("pole moment no wind %g ", pole_moment_per_wind);

        double effective_moment = resisting_moment * (1+config->wind_overdesign);
        double wind_pressure_failure = sqrt( effective_moment*effective_moment - 
            (wire_weight+equipment_weight+pole_moment)*(wire_weight+equipment_weight+pole_moment)) 
            / (pole_moment_per_wind+equipment_moment_nowind+wire_moment_nowind); // ignore wiree_tension
        critical_wind_speed = sqrt(wind_pressure_failure / (0.00256 * 2.24 * 2.24));
        verbose("wind_pressure_failure = %g psf (overdesign factor = %g)",wind_pressure_failure,config->wind_overdesign); // unit: pounds per square foot
        verbose("critical_wind_speed = %g m/s",critical_wind_speed);
        if ( wind_gusts > 0.0 )
        {
            double wind_gusts_failure = wind_gust_cdf(wind_gusts / critical_wind_speed);
            verbose("chance_of_withstanding = %g %% (wind_gusts = %g m/s)",wind_gusts_failure*100, wind_gusts);
        }

        pole_status = ( pole_stress < 1.0 ? PS_OK : PS_FAILED );
        verbose("pole_status = %d",pole_status);
        if ( pole_status == PS_FAILED )
        {
	    if ( stop_on_pole_failure )
	    {
                verbose("pole failed at %.0f%% stress, time to repair is %g h",pole_stress*100,repair_time);
	    }
	    else
	    {
                warning("pole failed at %.0f%% stress, time to repair is %g h",pole_stress*100,repair_time);
	    }
            down_time = gl_globalclock;
            verbose("down_time = %lld", down_time);
        }
        recalc = false;
	}
    else
    {
        verbose("no pole recalculation flagged");
    }

    if ( pole_status == PS_FAILED )
    {
        if ( stop_on_pole_failure )
        {
            return TS_INVALID;
        }
        TIMESTAMP next_event = down_time + (int)(repair_time*3600);
        if ( t0 == next_event )
        {
            return TS_NEVER;
        }
        verbose("next_event = %lld", next_event); //// should return repair time
        return next_event;

    }
    return TS_NEVER;
}

TIMESTAMP pole::commit(TIMESTAMP t1, TIMESTAMP t2)
{
    //  - pole          finalize pole status
    // TODO
    verbose("clearing pole recalculation flag");
	return TS_NEVER;
}
